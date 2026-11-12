# Datasheet — BusAccel-8266-v4

Documentación de usuario detallada del nodo acelerómetro de bus urbano.

---

## 1. Descripción general

El BusAccel-8266-v4 es un nodo de detección de golpes y aceleraciones para el
chasis de un bus urbano. Utiliza un acelerómetro ADXL345 (módulo GY-291)
conectado por I2C a una NodeMCU ESP8266. El firmware detecta eventos de
aceleración vertical (eje Z), mide el pico del evento en una ventana de ~2 s
y lo reporta por MQTT cuando el pico supera estrictamente 0.58 g.

**Características principales:**

- Detección de eventos por desviación del eje Z respecto a un setpoint
  automático (promedio de 20 lecturas).
- Ventana de captura de 100 muestras previas + 100 posteriores (~2 s a 100 Hz).
- Cálculo del pico = máximo `|desviación Z|` de la ventana.
- Publicación MQTT **condicional**: solo si el pico > 0.58 g (estricto).
- Salida serie **100 % silenciosa** salvo una línea por evento con el número
  del pico (`%.2f`).
- Heartbeat MQTT cada 30 s para mantener el estado "En línea".
- Watchdog de hardware de 8 s y watchdog de ping con reinicio por MAC
  aleatoria.
- Contador de eventos persistente en RTC user memory.
- Modo CSV a 20 Hz compatible con la app de escritorio v1/v2.

---

## 2. Especificaciones técnicas

### 2.1 Placa

| Parámetro | Valor |
|---|---|
| Modelo | NodeMCU ESP8266 (módulo ESP-12E) |
| MCU | Tensilica L106 (ESP8266EX), 32-bit |
| Frecuencia | 80/160 MHz |
| RAM | 80192 B usables (~80 KB) |
| IRAM | 65536 B |
| Flash | 4 MB |
| Voltaje de operación | 3.3 V |
| FQBN | `esp8266:esp8266:nodemcuv2` |
| Core | `esp8266:esp8266@3.1.2` |

### 2.2 Sensor

| Parámetro | Valor |
|---|---|
| Modelo | GY-291 / ADXL345 |
| Bus | I2C |
| Dirección I2C | `0x53` |
| Rango | ±2 g |
| Resolución | 256 LSB/g |
| Data rate | 100 Hz |
| Alimentación | 3.3 V (NUNCA 5 V) |

### 2.3 Conectividad

| Parámetro | Valor |
|---|---|
| WiFi | STA (modo estación) |
| Broker MQTT | `mqtt.example.com` |
| Puerto | `1886` |
| Autenticación | Client ID + usuario/clave |
| Client ID | `cliente-ejemplo-01` |
| Buffer MQTT | 384 B |
| Heartbeat | 30 s |
| Ping ICMP | 8.8.8.8 cada 30 s |

### 2.4 Umbrales y constantes

| Constante | Valor | Función |
|---|---|---|
| `ARROW_THRESHOLD_G` | 0.20 g | Detección de evento |
| `SEVERE_THRESHOLD_G` | 0.24 g | Clasificación de severidad |
| `RETURN_G` | 0.15 g | Histéresis de retorno a reposo |
| `PUBLISH_MIN_PEAK_G` | 0.58 g | Umbral de publicación MQTT (estricto) |
| `EVENT_LOCKOUT_MS` | 250 ms | Anulación de rebote |
| `REST_MS` | 50 ms | Reposo sostenido para desbloqueo |
| `PRE_N` | 100 muestras | Anillo del pasado (~1 s) |
| `POST_N` | 100 muestras | Ventana posterior (~1 s) |
| `SAMPLE_INTERVAL_MS` | 10 ms | Muestreo interno (~100 Hz) |
| `HEARTBEAT_INTERVAL_MS` | 30000 ms | Intervalo de heartbeat |
| `STALE_REPORT_MS` | 900000 ms (15 min) | Timeout de publicación |
| `PING_FAIL_ROUNDS` | 3 | Rondas sin ping antes de reiniciar |

---

## 3. Librerías requeridas

### 3.1 Librerías del core (incluidas con `esp8266:esp8266@3.1.2`)

| Librería | Propósito |
|---|---|
| `Wire.h` | Comunicación I2C con el ADXL345 |
| `math.h` | `fabsf()` |
| `ESP8266WiFi.h` | WiFi STA |
| `PubSubClient.h` | Cliente MQTT |
| `user_interface.h` | `wifi_set_macaddr` (dentro de `extern "C"`) |
| `ping.h` | ICMP a 8.8.8.8 (dentro de `extern "C"`) |

### 3.2 Librerías externas (instalar vía Arduino Library Manager)

| Librería | Versión | Propósito |
|---|---|---|
| Adafruit Unified Sensor | 1.1.14 | Interfaz base de sensores |
| Adafruit ADXL345 | 1.3.x | `begin()`, `setRange()`, `setDataRate()` |

**Instalación:**

```bash
arduino-cli lib install "Adafruit Unified Sensor"
arduino-cli lib install "Adafruit ADXL345"
```

O desde el IDE de Arduino: **Sketch → Include Library → Manage Libraries**,
buscar "Adafruit Unified Sensor" y "Adafruit ADXL345", e instalar.

---

## 4. Configuración de credenciales

El archivo `secrets.h` contiene las credenciales reales y **no se versiona**.
Para configurarlo:

1. Copiar `secrets.example.h` como `secrets.h`.
2. Rellenar los valores reales:

```cpp
#define WIFI_SSID     "TU_RED_2.4GHZ"
#define WIFI_PASSWORD "TU_PASSWORD_WIFI"

#define MQTT_BROKER   "mqtt.example.com"
#define MQTT_PORT     1886
#define MQTT_USERNAME "USUARIO_MQTT"
#define MQTT_PASSWORD "PASSWORD_MQTT"
#define MQTT_CLIENT   "cliente-ejemplo-01"
```

> **IMPORTANTE**: `MQTT_CLIENT` debe ser **único por placa**. El broker
> autoriza por Client ID; dos placas con el mismo ID se expulsan mutuamente de
> forma continua.

---

## 5. Contrato MQTT detallado

### 5.1 Topics

| Dirección | Topic | Payload | Descripción |
|---|---|---|---|
| Placa → Broker | `<MQTT_CLIENT>/report/online` | `"true"` | Heartbeat: al conectar y cada 30 s |
| Placa → Broker | `<MQTT_CLIENT>/report/value` | `"0.62"` (texto plano) | Pico del evento (solo si > 0.58 g) |
| Broker → Placa | `<MQTT_CLIENT>/directive/<cmd>` | — | Comandos (solo `RECAL`) |

### 5.2 Suscripción

La placa se suscribe a `<MQTT_CLIENT>/#` para recibir comandos.

### 5.3 Formato del payload de pico

```
0.62
```

- Valor en g con 2 decimales, **texto plano** (no JSON).
- Se publica **solo** si el pico supera estrictamente 0.58 g.
- Si el pico es ≤ 0.58 g, **no se publica nada** por MQTT.

### 5.4 Comandos

| Comando | Topic | Efecto |
|---|---|---|
| `RECAL` | `<MQTT_CLIENT>/directive/RECAL` | Recalibra el setpoint en caliente (sin reiniciar) |

### 5.5 Topics obsoletos

Los siguientes topics **ya no existen** en v4:

- ~~`<MQTT_CLIENT>/accel/setpoint`~~
- ~~`<MQTT_CLIENT>/accel/online`~~
- ~~`<MQTT_CLIENT>/accel/status`~~
- ~~`<MQTT_CLIENT>/accel/event/peak`~~
- ~~`<MQTT_CLIENT>/report/peak`~~ (sustituido por `report/value` en v4)

---

## 6. Salida serie

### 6.1 Formato

- **Velocidad**: 115200 baud.
- **Arranque**: 100 % silencioso (sin banner, sin diagnóstico, sin setpoint).
- **Evento**: una sola línea con solo el número del pico, formato `%.2f\n`.
- **Reposo**: silencio total.

### 6.2 Ejemplos

```
# Arranque: (nada)

# Golpe con pico 0.62 g:
0.62

# Golpe con pico 0.44 g (no publica, sí imprime):
0.44

# Reposo: (nada)
```

---

## 7. Modos de operación

### 7.1 Modo normal (`MODE_SERIAL_CSV 0`, `MQTT_DRY_RUN 0`)

- Detección de eventos por aceleración del eje Z.
- Publicación MQTT condicional (pico > 0.58 g).
- Heartbeat cada 30 s.
- Salida serie: una línea por evento con el pico.
- WiFi STA + broker MQTT real.

### 7.2 Modo CSV (`MODE_SERIAL_CSV 1`)

- Salida CSV a 20 Hz: `t,X,Y,Z,setX,setY,setZ,sym`.
- Sin WiFi ni MQTT.
- Compatible con la app de escritorio v1/v2.
- Formato idéntico a v1/v2.

### 7.3 Modo ensayo en seco (`MQTT_DRY_RUN 1`)

- No se abre WiFi ni MQTT.
- Los envíos son silenciosos (sin eco por serie).
- La línea serie del pico se imprime normalmente.
- Útil para validar la detección sin suscriptor en el broker.

---

## 8. Guía de solución de problemas

### 8.1 La placa no imprime nada en el arranque

**Es comportamiento esperado.** El arranque es 100 % silencioso. No se
imprime nada durante el arranque, la conexión WiFi/MQTT, la adquisición del
setpoint ni el diagnóstico del sensor. La primera línea solo aparece tras el
primer evento detectado.

### 8.2 No se publica por MQTT

Verificar:

1. **Umbral de publicación**: el pico debe superar estrictamente 0.58 g. Si
   el pico es ≤ 0.58 g, no se publica (pero sí se imprime por serie).
2. **Conexión WiFi**: verificar que el SSID y la contraseña en `secrets.h`
   son correctos.
3. **Conexión MQTT**: verificar que el broker `mqtt.example.com:1886` es
   alcanzable y que el Client ID está autorizado.
4. **Heartbeat**: si no se recibe `report/online` cada 30 s, la placa no está
   conectada al broker.

### 8.3 La placa se reinicia sola

Es el comportamiento de los watchdogs:

- **Watchdog de ping**: si tras 3 rondas consecutivas (cada 30 s) no hay
  respuesta de ping a 8.8.8.8 y el broker no es alcanzable, la placa se
  reinicia con MAC aleatoria.
- **Stale publish**: si no se logra publicar nada en 15 min, la placa se
  reinicia con MAC aleatoria.
- **Watchdog de hardware**: `WDTO_8S`. Si el loop se bloquea más de 8 s, la
  placa se reinicia.

Verificar la conectividad de red y el broker.

### 8.4 El sensor no responde por I2C

En el banco de pruebas, el sensor a veces no responde por I2C (latch del bus).
El firmware lo reintenta cada 1 s en background, silenciosamente. **No es un
bug**. Si el problema persiste:

1. Verificar las conexiones SDA/SCL.
2. Verificar que el sensor está alimentado a 3.3 V.
3. Verificar que hay pull-ups en el bus I2C (el módulo GY-291 los incluye).

### 8.5 Dos placas se patean mutuamente

Es porque tienen el mismo Client ID. Cambiar `MQTT_CLIENT` en `secrets.h` de
una de las placas para que sea único.

### 8.6 El setpoint no es correcto

- Asegurarse de que el bus está en reposo durante los primeros ~200 ms tras
  el arranque.
- Si el ángulo de montaje cambia, enviar `RECAL` para recalibrar en caliente.
- El setpoint se recalcula silenciosamente cada 20 muestras en reposo.

### 8.7 No se reciben eventos

Verificar:

1. Que el sensor está conectado y alimentado.
2. Que el umbral de detección (0.20 g) es adecuado para la aplicación.
3. Que el bus no está en vibración constante (el setpoint se adaptaría y los
   eventos no superarían el umbral).

---

## 9. Compatibilidad con otras placas Arduino

| Placa | Compatibilidad | Notas |
|---|---|---|
| NodeMCU ESP8266 (ESP-12E) | **Nativa** | Placa objetivo. FQBN `esp8266:esp8266:nodemcuv2` |
| NodeMCU ESP8266 (ESP-12F) | **Nativa** | Mismo core, mismo pinout |
| Wemos D1 mini | **Nativa** | Mismo core; verificar pines D1/D2 |
| ESP32 | No compatible | Requeriría cambios en el core y posiblemente en el pinout |
| Arduino Uno/Nano | No compatible | Requeriría reescritura (WiFi, MQTT, RTC user memory) |

> El firmware está diseñado específicamente para el ESP8266. No es portable
> directamente a otras arquitecturas sin modificaciones significativas.

---

## 10. Seguridad

- **Credenciales**: `secrets.h` contiene las credenciales reales y **no debe
  versionarse**. El `.gitignore` ya lo excluye.
- **Client ID único**: el broker autoriza por Client ID. No compartir el mismo
  Client ID entre placas.
- **Alimentación**: el sensor debe alimentarse a 3.3 V. No aplicar 5 V.
- **Comandos MQTT**: el único comando implementado es `RECAL`. No hay
  comandos de reinicio ni de configuración remota.

---

## 11. Historial de versiones

| Versión | Cambios principales |
|---|---|
| v1 | Detección básica por evento, salida serie con símbolos |
| v2 | App de escritorio, modo CSV, contador RTC |
| v3 | Arranque con diagnóstico, publicación MQTT de setpoint/status |
| **v4** | **Publicación condicional (pico > 0.58 g), arranque 100 % silencioso, salida serie reducida a una línea con el pico, contrato MQTT `report/`, heartbeat 30 s, watchdog de ping** |
