# Prueba local del ESP con RabbitMQ

Esta prueba usa un RabbitMQ aislado con MQTT, como el protocolo del proyecto. No conecta con el backend ni con la central real. El usuario y contraseña `admin_admin` / `admin` son únicamente para una red de prueba confiable; el puerto MQTT queda accesible en la red local y no usa cifrado. No usar con mercadería ni exponerlo a Internet.

## 1. Preparar la computadora

Abrir Docker Desktop y esperar a que esté iniciado. Abrir Terminal en esta carpeta y ejecutar:

```sh
cd prueba_mqtt
docker compose up -d rabbitmq
docker compose logs --tail 20 rabbitmq
```

La primera vez Docker descarga la imagen. Esperar hasta que el servidor haya terminado de arrancar. El panel de RabbitMQ queda disponible solo en esta computadora en http://localhost:15672 con el usuario y contraseña de prueba. El broker MQTT escucha en el puerto 1883 de la computadora. La primera vez que se usen los clientes MQTT Docker descargará también su imagen.

## 2. Preparar el ESP8266

Cargar el firmware y configurarlo con el portal como indica [`../dispensador/LEEME.md`](../dispensador/LEEME.md). En el portal, usar la IP de la computadora: en macOS está en Ajustes del Sistema → Wi-Fi → Detalles. No usar `localhost`. El ESP8266 usa Wi-Fi de 2,4 GHz y la computadora debe aceptar conexiones al puerto 1883 en el firewall. La pantalla indica `Esperando orden` cuando conecta.

## 3. Escuchar los eventos

En una terminal, desde esta carpeta:

```sh
docker compose run --rm mqtt -h rabbitmq -u admin_admin -P admin -t 'devices/DSP-01/dispenser/event' -v
```

Dejar esa terminal abierta. Abrir una segunda terminal en la misma carpeta para enviar mensajes. Si la red de la computadora no permite descargar la imagen del cliente, resolverlo antes de continuar.

## 4. Probar botón de carga

En la segunda terminal:

```sh
cd prueba_mqtt
docker compose run --rm --entrypoint mosquitto_pub mqtt -h rabbitmq -u admin_admin -P admin -t 'devices/DSP-01/dispenser/cmd' -m '{"cmd":"start","request_id":"PRUEBA-1:0","order_id":"PRUEBA-1","vehicle_id":"VHC-001"}'
```

La terminal que escucha debería mostrar `started`. Presionar RIGHT: debería mostrar `finished`, con el mismo `request_id`.

## 5. Probar botón sin stock

```sh
docker compose run --rm --entrypoint mosquitto_pub mqtt -h rabbitmq -u admin_admin -P admin -t 'devices/DSP-01/dispenser/cmd' -m '{"cmd":"start","request_id":"PRUEBA-2:0","order_id":"PRUEBA-2","vehicle_id":"VHC-001"}'
```

Debería aparecer `started`. Presionar LEFT: debería aparecer `error` con `code` igual a `EMPTY` y el mismo `request_id`. Si no aparece `started`, revisar que la pantalla indique `Esperando orden`, que la IP sea correcta, la red y el firewall. Si aparece `started` pero no el resultado, revisar en el monitor serie los valores `A0=...` de los botones.

Al terminar, cerrar el suscriptor con Ctrl+C y ejecutar `docker compose down`. Esta prueba no sustituye la integración: el firmware guarda el estado solo en memoria, publica con QoS 0 y no recibe cancelación.

## Diferencia con el Docker de pruebas del backend

El archivo `e2e/docker-compose.e2e.yml` del backend configura RabbitMQ de prueba con usuario `admin_admin` y contraseña `admin`, y habilita el plugin MQTT. Ese archivo publica el panel de administración, pero no publica el puerto MQTT hacia la computadora; para conectar una placa externa hay que exponerlo y comprobar el acceso de red. Son credenciales de desarrollo, no necesariamente las del servidor de la facultad.
