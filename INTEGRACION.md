# Integración con Warehouse USAL — revisión de código

Revisado el 3 de noviembre de 2026, a partir de estas versiones públicas:

- Backend `Warehouse-USAL/wh-backend`, rama `develop`, commit `b60edcdea96fd2c4cb8d03456e4ae458efdfa980`.
- Central `Warehouse-USAL/G5_Version_NET`, rama `master`, commit `2c436c55bf61fc3b06ce7f29b900f05a720949b4`. La otra rama publicada, `queue-redundancia`, tampoco contiene implementación fuente de dispensadores.
- `Warehouse-USAL/WH-Infra` es otra implementación anterior; la documentación del backend identifica `G5_Version_NET` como central actual.

## Resultado

**El prototipo no está integrado de extremo a extremo.** El firmware implementa los mensajes del dispensador descritos en el `PROTOCOLO.md` suministrado, pero la central publicada todavía no implementa ese contrato. En `Orchestrator.cs`, el filtro de mensajes de dispositivos admite solo `rover` y `camera` y, cuando llega a una parada, `StartDwellAsync` espera `DwellSec` y continúa. En `MqttGateway.cs` no hay suscripciones a eventos de dispensador. No se encontró código fuente que mande `start`, espere `started`/`finished` o procese `error`. Por lo tanto, el rover puede avanzar y completar una orden sin esperar al operario. El firmware por sí solo no puede corregir ese comportamiento.

La central que se ejecute en el depósito podría ser otra versión: hay que confirmar su commit o conseguir el código fuente de la versión compatible con el protocolo. El archivo `PROTOCOLO.md` recibido describe capacidades que no aparecen en la versión pública inspeccionada.

## Recorrido real de los mensajes

1. El backend genera la orden en Kafka `orders`: `order_id`, `status`, `stops`, `destination` y `published_at`.
2. El bridge la reenvía a RabbitMQ, donde la central la recibe en MQTT `orders`.
3. La central controla rover y cámara. Según el protocolo entregado, una central compatible debería enviar `start` a `devices/DSP-01/dispenser/cmd` al llegar a una parada asociada al dispensador, y esperar su respuesta.
4. El ESP responde `started`; el botón de carga produce `finished` y el botón sin stock produce `error` con `EMPTY`, en `devices/DSP-01/dispenser/event`. Es la central la que debe interpretar esos eventos y decidir cómo continuar o cancelar.
5. La central informa el estado de la orden o telemetría en `vehicles`. El bridge lo traduce a `POST /infra/orders/{id}/status` o `POST /infra/vehicles/{id}/telemetry` con `X-Api-Key`. El dispensador no debe llamar directamente esos endpoints ni declarar la orden completada: todavía pueden quedar otras paradas y la vuelta del rover.

## Información y seguridad pendientes

- Corrección: el backend sí ofrece `GET /orders/{id}` y `GET /orders`, con productos, SKU y cantidades totales de la orden; `GET /products/{id}` devuelve el nombre. Estas rutas requieren JWT. También existe `GET /warehouse/positions/{id}` con producto y QR, restringido a administradores. Lo que no trae SKU ni cantidades es el mensaje MQTT `orders` ni el `start` del protocolo suministrado. No se verificó un endpoint que devuelva la cantidad asignada a cada parada específica. Hay que acordar cómo consultar y transmitir esa información al dispositivo.
- Se requiere asociación de QR con dispensador, espera del resultado, cancelación y expiración explícitas, y recuperación tras reinicios en la central y el dispositivo. El firmware actual no recibe una cancelación, guarda el estado solo en RAM y publica con QoS 0 por usar PubSubClient. No debe utilizarse para mover mercadería real como si garantizara entrega o ausencia de duplicados.
- El timeout de finalización indicado en el protocolo es 60 segundos; se debe acordar un valor que alcance para la tarea manual.
- Los identificadores largos se recortan a 16 caracteres en el LCD del ESP. Un dato abreviado puede ser ambiguo; se necesita presentación completa o desplazamiento antes de usarlo operacionalmente.

## Pruebas realizadas y pendientes

La revisión anterior comprobó en Tinkercad la interfaz y ambos botones con órdenes simuladas. En esta revisión se inspeccionaron las implementaciones y tests del backend, bridge y central; **no** se ejecutó una prueba real entre ellos y el ESP. Docker está instalado, pero su daemon no estaba activo; tampoco se detectaron `dotnet`, `go` ni `arduino-cli` en el PATH. La prueba de integración necesita la central con soporte para dispensador, broker de prueba, firmware compilado y el ESP (o un simulador MQTT del dispensador), además del backend y bridge configurados.

Fuentes: [contrato del backend](https://github.com/Warehouse-USAL/wh-backend/issues/128), [bridge](https://github.com/Warehouse-USAL/wh-backend/blob/b60edcdea96fd2c4cb8d03456e4ae458efdfa980/bridge/README.md), [orquestador de Infra](https://github.com/Warehouse-USAL/G5_Version_NET/blob/2c436c55bf61fc3b06ce7f29b900f05a720949b4/EmergentesProyecto/Orchestrator.cs), [suscripciones MQTT](https://github.com/Warehouse-USAL/G5_Version_NET/blob/2c436c55bf61fc3b06ce7f29b900f05a720949b4/EmergentesProyecto/MqttGateway.cs).

## ¿El equipo del dispensador automático ya tiene todo?

No se puede confirmar el estado actual de su trabajo: solo revisamos el archivo de ejemplo recibido, no una versión final de ese equipo. Ese archivo ya contiene conexión Wi-Fi y MQTT, recepción de `start`, eventos `started`/`finished`/`error` y telemetría. Sin embargo, deja vacía `iniciarDespacho()` y `despachoTerminado()` devuelve `true` inmediatamente. Por eso, tal como está, puede informar que terminó sin mover ni verificar ningún producto. Ellos deben confirmar si ya implementaron y probaron esas funciones, el mecanismo, la detección de fin y de errores.

Además, tanto el automático como el manual dependen de una central que implemente el contrato. La versión pública revisada no lo hace; podría existir otra versión. Por último, el ejemplo automático solo recuerda la última operación terminada en memoria y publica eventos con QoS 0; hace falta acordar cómo evitar duplicaciones y recuperar eventos ante cortes o reinicios antes de usarlo con mercadería real.

## Lo que ya se pudo determinar en el código

- Al crear una orden, el backend calcula las posiciones a visitar según el stock y guarda sus QR ordenados por recorrido. El cálculo utiliza una selección FIFO de posiciones. El mensaje publicado contiene esos QR, pero no conserva ni comunica la cantidad asignada a cada parada. Las posiciones sin QR se omiten del recorrido. Fuentes: [StopResolver](https://github.com/Warehouse-USAL/wh-backend/blob/b60edcdea96fd2c4cb8d03456e4ae458efdfa980/backend/src/main/java/com/usal/whbackend/service/StopResolver.java) y [StockDrainService](https://github.com/Warehouse-USAL/wh-backend/blob/b60edcdea96fd2c4cb8d03456e4ae458efdfa980/backend/src/main/java/com/usal/whbackend/service/StockDrainService.java).
- Al marcar la orden `completed`, el backend descuenta stock de los artículos. El camino de cambio de estado a `cancelled` solo cambia el estado; no registra en ese paso una entrega parcial. El descuento vuelve a calcular posiciones, por lo que no debemos suponer que se registró una asignación fija por parada. Fuente: [OrderService](https://github.com/Warehouse-USAL/wh-backend/blob/b60edcdea96fd2c4cb8d03456e4ae458efdfa980/backend/src/main/java/com/usal/whbackend/service/OrderService.java).
- El protocolo ya define el funcionamiento esperado: asociación QR/dispositivo en el panel de la central, espera de 60 segundos, reenvío del inicio tras 5 segundos y espera de 180 segundos si el dispensador está desconectado. No hace falta volver a pedir esas especificaciones; falta comprobar que correspondan a la central que realmente se ejecute y que el tiempo alcance para el operario.
- Revisamos ambas ramas publicadas de la central y el único PR listado. La implementación de dispensadores descrita en el protocolo no aparece en el código fuente publicado que inspeccionamos. Esta conclusión se limita a esas versiones; no permite descartar código privado o una versión no publicada.

## Qué se puede demostrar con lo disponible

El circuito de Tinkercad permite demostrar la interacción del operario con órdenes de ejemplo: un botón confirma la carga y el otro informa falta de stock. No demuestra comunicación con MQTT, la central ni el backend. Para una demostración controlada sin cambios de código se puede usar una orden de un solo producto y una sola parada con cantidad conocida de antemano; la información necesaria para cargarla debe estar a la vista del operario por un medio externo. Es una demostración de flujo, no una solución general para órdenes de varios productos o posiciones.

El ESP de prueba habla el formato del protocolo para confirmar o informar `EMPTY`, pero necesita una central compatible, broker y hardware configurados. No se comprobó una conexión real ni una compilación física. Antes de usar mercadería real siguen sin resolverse la presentación inequívoca de producto y cantidad, la caducidad de la operación y la recuperación segura ante reinicios o cortes.

## Únicos elementos externos que faltan para cerrar la integración

1. **La versión ejecutable y el código de la central con soporte de dispensadores**, si existe otra versión distinta a las dos ramas revisadas. Sin esa implementación no podemos verificar que el rover espere a los botones o al mecanismo automático.
2. **Un entorno de prueba accesible:** broker y configuración de conexión, credenciales mediante canal seguro, IDs y asociación QR/dispositivo, y un ESP o simulador MQTT. Son datos necesarios para conectar y probar; no tienen que compartirse públicamente.
3. **El firmware automático actualizado, si existe**, para verificar si ya acciona el mecanismo y detecta su final. El ejemplo recibido no lo hace.
4. **Un modo fiable de proporcionar producto y cantidad por parada.** El código revisado no envía esos datos al dispositivo. Para una prueba de un solo producto pueden conocerse de antemano; para la solución general se requiere que estén disponibles y lleguen a quien prepara la carga.

No hace falta solicitar confirmación de los endpoints, de los tiempos definidos en el protocolo ni de cómo el backend calcula las paradas: ya fueron verificados en los archivos disponibles. Si la central compatible o los datos por parada no existen, implementarlos requerirá cambios de código por parte de quienes tienen permisos.

## Prueba integral que queda preparada para ejecutar

Con entorno de prueba, central compatible y dispositivos configurados:

1. Crear una orden de prueba con una parada y datos conocidos; verificar que backend y bridge la entregan a la central.
2. Hacer llegar el rover al QR asociado y observar `start` con un `request_id` en el topic del dispensador correcto; comprobar que el rover permanece detenido.
3. En el caso manual, confirmar la carga con el botón; en el automático, verificar físicamente el despacho. Observar `started` y después `finished` con el mismo ID y comprobar que recién entonces continúa el rover.
4. Repetir con falta de stock: observar `error`/`EMPTY`, verificar la acción acordada de la central y el estado final de la orden y del inventario en backend.
5. Repetir con dos paradas y con reconexión, reinicio y `start` duplicado; comprobar que no se despacha dos veces y que una orden no se marca completa antes de tiempo.

Estas son pruebas pendientes, no resultados ya obtenidos. Si falla alguna, guardar fecha, versión de cada componente, `request_id` y mensajes observados, sin registrar contraseñas.

La guía operativa y las llamadas HTTP preparadas están en [PRUEBA_REAL.md](PRUEBA_REAL.md).
