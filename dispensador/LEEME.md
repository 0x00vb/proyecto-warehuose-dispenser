# Dispensador manual: LCD Keypad Shield, D1 R1, portal y RabbitMQ

El firmware `dispensador.ino` usa una WeMos D1 R1 con un LCD Keypad Shield, Wi‑Fi y MQTT. Compilar con la placa `esp8266:esp8266:d1` y las bibliotecas PubSubClient, ArduinoJson 7 y WiFiManager.

## Botones

| Botón | Cuándo | Qué hace |
|---|---|---|
| RIGHT | con un pedido en pantalla | Cargado: manda `finished` |
| LEFT | con un pedido en pantalla | Sin stock: manda `error` con código `EMPTY` |
| UP, mantener 3 s | sin pedido | Abre el portal Wi‑Fi |

Ningún botón hace nada fuera de esos casos. El pedido vence a los 60 s sin respuesta, igual que en la central, y la pantalla muestra `Ult: VENCIDO`. Si se pulsa RIGHT o LEFT sin conexión, el resultado queda guardado y se manda al reconectar; mientras tanto la pantalla muestra `Enviando...`. Si la placa se reinicia, se pierde.

## Conexión

La primera vez, o si la configuración guardada no es válida, la pantalla muestra una red `DSP-...` y su clave. Desde el celular, entrar a esa red y abrir `http://192.168.4.1` si no aparece la página sola. Elegir el Wi‑Fi de 2,4 GHz y completar los datos del broker: IP de la computadora con RabbitMQ en la misma red, puerto 1883, usuario `admin_admin`, clave `admin` e ID `DSP-01`. Al guardar, la placa se reinicia; si conecta, la pantalla indica `Esperando orden`. Si no, indica `Sin WiFi` o `Sin servidor`, y con UP 3 s se corrige.

El portal dura 3 minutos y después reinicia sin cambios. Las credenciales se guardan sin cifrar en la placa y el broker de prueba no usa TLS.

## Levantar el broker

Abrir Docker Desktop. Desde la raíz del repo:

```sh
cd prueba_mqtt
docker compose up -d rabbitmq
```

La central pública (`G5_Version_NET`) todavía no manda `start` a los dispensadores, así que los pedidos de prueba se envían a mano al broker.

## Probar los botones

En una terminal, dejar escuchando eventos:

```sh
cd prueba_mqtt
docker compose run --rm mqtt -h rabbitmq -u admin_admin -P admin -t 'devices/DSP-01/dispenser/event' -v
```

En otra terminal, enviar un pedido:

```sh
cd prueba_mqtt
docker compose run --rm --entrypoint mosquitto_pub mqtt -h rabbitmq -u admin_admin -P admin -t 'devices/DSP-01/dispenser/cmd' -m '{"cmd":"start","request_id":"PRUEBA-1:0","order_id":"PRUEBA-1","vehicle_id":"VHC-001"}'
```

Debe aparecer `started` y la pantalla debe mostrar `PRUEBA-1` y `RIGHT=OK LEFT=NO`. Al pulsar RIGHT aparece `finished` con el mismo ID. Repetir con `PRUEBA-2` y LEFT para ver `error` `EMPTY`. Si se reenvía `PRUEBA-1`, la placa repite `finished` sin pedir otra carga.

## Hardware

Los botones se leen por A0 con umbrales fijos, pensados para el shield común con A0 de hasta 3,2 V: RIGHT ≈ 0, UP ≈ 230, LEFT ≈ 790 y suelto ≈ 1023. El monitor serie (115200) imprime `A0=...` cada vez que cambia la tecla, para comprobarlos una vez. Si LEFT no cae entre 650 y 900, ajustar `keyFromA0`. **No se ha medido el voltaje del shield en A0 ni verificado la compatibilidad eléctrica con la D1.** Hay que medir las señales y confirmar si hace falta adaptación de nivel.
