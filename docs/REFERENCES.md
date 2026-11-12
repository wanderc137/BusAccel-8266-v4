# Referencias — BusAccel-8266-v4

Fuentes que sostienen las decisiones de hardware, firmware y protocolo del nodo. Cada entrada indica para qué sirve y qué parte del proyecto la usa.

---

## Sensor y acondicionamiento

### Analog Devices — ADXL345
- **Producto:** https://www.analog.com/en/products/adxl345.html
- **Datasheet (PDF):** https://www.analog.com/media/en/technical-documentation/data-sheets/ADXL345.pdf
- **Uso:** escala ±2 g, resolución de 10 bits en el modo usado, registro `DATA_FORMAT`, `BW_RATE`, `POWER_CTL`, lectura de los seis bytes de ejes y conversión de cuentas a g (`× 0,0039 g/LSB`).
- **Estado:** ✅ Fuente primaria del principio físico y de los valores del driver.

### NXP — I2C-bus specification (UM10204)
- **Documento:** https://www.nxp.com/docs/en/user-guide/UM10204.pdf
- **Uso:** pull-ups, direcciones de 7 bits, condiciones de start/stop y tolerancia del bus a 400 kHz.
- **Estado:** ✅ Explica por qué el GY-291 necesita pull-ups y por qué SDA/SCL no se intercambian.

### Adafruit ADXL345
- **Librería:** https://github.com/adafruit/Adafruit_ADXL345
- **Guía:** https://learn.adafruit.com/adxl345-digital-accelerometer
- **Uso:** API `begin()`, `setRange()`, `getEvent()` y calibración práctica. El sketch usa la librería y el bus `Wire`.
- **Estado:** ✅ Dependencia real del firmware.

### Adafruit Unified Sensor
- **Repositorio:** https://github.com/adafruit/Adafruit_Sensor
- **Uso:** capa común que exige `Adafruit_ADXL345`; sin ella el compilador no encuentra `Adafruit_Sensor.h`.
- **Estado:** ✅ Dependencia transitiva.

---

## ESP8266 y herramientas

### ESP8266 Arduino Core
- **Documentación:** https://arduino-esp8266.readthedocs.io/
- **Repositorio:** https://github.com/esp8266/Arduino
- **Uso:** `Wire.begin(4, 5)`, `ESP8266WiFi`, `WiFiClient`, `millis()`, modos de sueño y particularidades del GPIO.
- **Estado:** ✅ Core con el que se compila (`esp8266:esp8266:nodemcuv2`).

### Arduino CLI
- **Documentación:** https://arduino.github.io/arduino-cli/
- **Instalación del core:** https://arduino.github.io/arduino-cli/latest/getting-started/
- **Uso:** compilar y cargar el sketch sin IDE; comandos reproducibles en el README.
- **Estado:** ✅ Herramienta recomendada.

### NodeMCU / ESP-12E pinout
- **Referencia:** https://randomnerdtutorials.com/esp8266-pinout-reference-gpios/
- **Uso:** aclarar que `D1`/`D2` son GPIO5/GPIO4 y no los pines analógicos de un UNO; evitar conflictos con el boot.
- **Estado:** ✅ Verificado contra la placa usada.

---

## MQTT y red

### MQTT 3.1.1 — OASIS Standard
- **Especificación:** https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html
- **Uso:** QoS 0, retain, keepalive, Last Will and Testament y formato de topics.
- **Estado:** ✅ Protocolo que implementa `PubSubClient`.

### PubSubClient
- **Repositorio:** https://github.com/knolleary/pubsubclient
- **Documentación:** https://pubsubclient.knolleary.net/
- **Uso:** `setServer()`, `setCallback()`, `connect()` con usuario/password y `publish()`; límite de buffer de 384 B configurado en el sketch.
- **Estado:** ✅ Dependencia real.

### Eclipse Mosquitto
- **Documentación:** https://mosquitto.org/documentation/
- **Cliente `mosquitto_pub`:** https://mosquitto.org/man/mosquitto_pub-1.html
- **Uso:** probar el contrato MQTT y enviar el comando `RECAL` sin escribir un cliente propio.
- **Estado:** ✅ Herramienta de verificación.

### WiFi 2,4 GHz en ESP8266
- **Nota oficial:** https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/readme.html
- **Uso:** el ESP8266 no soporta 5 GHz; el SSID debe ser una red de 2,4 GHz.
- **Estado:** ✅ Limitación documentada en el README.

---

## Concepto y mantenimiento vial

### Aceleración específica y unidades g
- **Referencia:** https://en.wikipedia.org/wiki/Standard_gravity
- **Uso:** definir 1 g = 9,80665 m/s² y explicar por qué el firmware trabaja en g y no integra la señal.
- **Estado:** 📚 Contexto, no dependencia.

### Acelerómetro MEMS capacitivo
- **Referencia:** https://en.wikipedia.org/wiki/Accelerometer
- **Uso:** masa suspendida, dedos capacitivos y conversión analógica-digital; sección de principio físico del README.
- **Estado:** 📚 Contexto.

### OBD-II y mantenimiento basado en condición
- **Referencia:** https://en.wikipedia.org/wiki/On-board_diagnostics
- **Uso:** analogía de por qué un evento de aceleración es un dato de mantenimiento y no solo una alerta.
- **Estado:** 📚 Contexto del caso de uso.

---

## Cómo se usaron estas fuentes

| Decisión | Fuente principal |
|---|---|
| Escala ±2 g y conversión a g | Datasheet ADXL345 |
| `Wire.begin(4, 5)` en NodeMCU | ESP8266 Arduino Core + pinout |
| Buffer MQTT de 384 B | PubSubClient + medición del payload |
| QoS 0 y retain | MQTT 3.1.1 |
| Comando `RECAL` | Contrato propio, probado con `mosquitto_pub` |
| Umbral 0,58 g | Medición de campo, no una constante de datasheet |

Las fuentes externas explican el hardware y el protocolo; los umbrales de evento, el setpoint automático y la ventana de captura son decisiones de este proyecto y están documentadas en el README y en `docs/developer.md`.
