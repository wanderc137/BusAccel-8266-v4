# Developer — BusAccel-8266-v4

Documentación técnica del firmware para desarrolladores.

---

## 1. Arquitectura del sketch

El sketch `BusAccel-8266-v4.ino` (~915 líneas) está organizado en bloques
funcionales claros:

```
1.  Cabecera: descripción, mapa de pines, contrato de datos, changelog v3→v4
2.  Directivas de compilación: MODE_SERIAL_CSV, MQTT_DRY_RUN
3.  Includes: Wire, math, Adafruit_Sensor, Adafruit_ADXL345_U
      (condicionales a MODE 0: ESP8266WiFi, PubSubClient, user_interface.h,
       ping.h, secrets.h)
4.  Constantes: sensor/muestreo, detección, ventana (PRE_N/POST_N), red,
      PUBLISH_MIN_PEAK_G
5.  Pines y registros ADXL345
6.  struct sample_t { int16_t x,y,z; uint16_t dt; }
7.  Estado global: sensor, setpoint, detección, ventana (preRing), red
8.  Prototipos
9.  Acceso a registros por Wire + diagnóstico (SILENCIOSO)
10. Contador RTC (load/save con validación reforzada)
11. pushSample()  — anillo del pasado
12. beginCapture() — escaneo del anillo para el pico inicial
13. advanceCapture() — actualiza pico en cada muestra POST
14. finishCapture() — evalúa umbral de publicación + imprime UNA línea
15. publishPeakEvent() — publica SOLO si peak > PUBLISH_MIN_PEAK_G
16. processSample() — máquina de estados IDLE/LOCKED (SILENCIOSA)
17. printCharTick() — CSV 20 Hz (solo MODE 1)
18. Red: WiFi/MQTT/ping (solo MODE 0, SILENCIOSO en arranque)
19. setup() / loop()
```

---

## 2. setup() — Arranque silencioso

```cpp
void setup() {
  ESP.wdtEnable(WDTO_8S);          // 1. Watchdog primero
  Serial.begin(115200);            // 2. Serie (sin banner)
  memset(preRing, 0, sizeof(preRing));  // 3. Anillo limpio

  // 4. Cargar contador RTC (validación reforzada)
  loadEventCounter();

  // 5. Detectar reinicio por fallo (magic 0xDEAD) → randomizeMac()
  rtcData_t rtc;
  ESP.rtcUserMemoryRead(0, (uint32_t*)&rtc, sizeof(rtc));
  if (rtc.magic == 0xDEAD && rtc.failureFlag == 1) {
    randomizeMac();
    rtc.failureFlag = 0;
    ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtc, sizeof(rtc));
  }

  // 6. Inicializar I2C y sensor
  Wire.begin(PIN_SDA, PIN_SCL);    // Wire.begin(4, 5)
  if (accel.begin()) {
    accel.setRange(SENSOR_RANGE);       // ±2 g
    accel.setDataRate(SENSOR_DATARATE); // 100 Hz
    sensorOnline = true;
  }

  // 7. Diagnóstico silencioso (forzar measurement mode)
  runChipDiagnostic();

  // 8. Iniciar red (solo MODE 0 && !MQTT_DRY_RUN)
  startWifi();
}
```

**Punto clave**: el arranque es 100 % silencioso. No se imprime nada por
serie durante el setup.

---

## 3. loop() — Estructura principal

```cpp
void loop() {
  ESP.wdtFeed();
  unsigned long now = millis();

  // 1) MUESTREO PRIMERO (~100 Hz) — tiene prioridad sobre la red
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    uint16_t dt = (uint16_t)(now - lastSampleMs);
    lastSampleMs = now;
    if (readAccel()) {
      pushSample(dt);       // 1) anillo del pasado (siempre)
      processSample();      // 2) detección -> puede abrir ventana
      advanceCapture(dt);   // 3) si hay ventana, actualiza pico
    }
  }

  // 2) Reintento del sensor en background (si no responde)
  if (!sensorOnline && (now - lastRetryMs >= SENSOR_RETRY_MS)) {
    lastRetryMs = now;
    Wire.begin(PIN_SDA, PIN_SCL);
    if (accel.begin()) { /* ... */ }
  }

  // 3) RED DESPUÉS (solo MODE 0 && !MQTT_DRY_RUN) — no bloqueante
  serviceWifi();
  serviceMqtt();
  client.loop();
  servicePing();

  // 4) Heartbeat cada 30 s
  if (client.connected() && (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)) {
    lastHeartbeatMs = now;
    sendToBroker("report/online", "true");
  }

  // 5) Stale publish check (cada ~1 s)
  if (now - lastStaleCheckMs >= 1000UL) {
    lastStaleCheckMs = now;
    if ((now - lastGoodPublish) > STALE_REPORT_MS) {
      rebootWithNewMac();
    }
  }
}
```

**El orden de las tres llamadas de muestreo es crítico**:
- `pushSample()` antes de `processSample()` para que la muestra que dispara
  ya esté en el anillo.
- `processSample()` antes de `advanceCapture()` para que `devZ` esté fresco
  al medir el pico.

---

## 4. Funciones de detección y captura

### 4.1 pushSample(uint16_t dt)

Escribe la muestra actual en el anillo `preRing`. Se llama en **cada** muestra,
evento o no. Es lo que permite medir el pico de lo ocurrido antes del cruce.

```cpp
void pushSample(uint16_t dt) {
  preRing[ringIdx].x  = rawX;
  preRing[ringIdx].y  = rawY;
  preRing[ringIdx].z  = rawZ;
  preRing[ringIdx].dt = dt;
  ringIdx = (uint8_t)((ringIdx + 1) % PRE_N);
}
```

### 4.2 beginCapture()

Se llama cuando se detecta un evento (transición ST_IDLE → ST_LOCKED). Escanea
el anillo `preRing` para medir el pico inicial de lo ocurrido **antes** del
cruce.

```cpp
void beginCapture() {
  eventDevZ  = devZ;
  eventSym   = eventChar;
  eventPeakZ = fabsf(devZ);
  for (uint8_t i = 0; i < PRE_N; i++) {
    float d = fabsf(preRing[i].z / 256.0f - spZ);
    if (d > eventPeakZ) eventPeakZ = d;
  }
  postCount    = 0;
  collecting   = true;
  eventStartMs = millis();
  eventCount++;
  saveEventCounter();  // consume id al abrir (ids sin huecos)
}
```

### 4.3 advanceCapture(uint16_t dt)

Actualiza el pico con cada muestra POST y cuenta. Al llegar a `POST_N` cierra
la ventana llamando a `finishCapture()`.

```cpp
void advanceCapture(uint16_t dt) {
  (void)dt;
  if (!collecting) return;
  float d = fabsf(devZ);
  if (d > eventPeakZ) eventPeakZ = d;
  postCount++;
  if (postCount >= POST_N) {
    collecting = false;
    finishCapture();
  }
}
```

### 4.4 finishCapture()

Cierra la ventana del evento. Evalúa el umbral de publicación MQTT e imprime
una línea serie con el pico.

```cpp
void finishCapture() {
  // 1. Publicación MQTT CONDICIONAL
  if (eventPeakZ > PUBLISH_MIN_PEAK_G) {  // 0.58 g estricto
    publishPeakEvent();
  }
  // 2. Salida serie: UNA sola línea con SOLO el número del pico
  Serial.printf("%.2f\n", eventPeakZ);
}
```

### 4.5 publishPeakEvent()

Publica el pico por MQTT. Solo se invoca cuando `eventPeakZ > PUBLISH_MIN_PEAK_G`.

```cpp
void publishPeakEvent() {
  char msg[16];
  snprintf(msg, sizeof(msg), "%.2f", eventPeakZ);
  if (!client.connected()) return;
  sendToBroker("report/value", msg);  // texto plano, no JSON
}
```

### 4.6 processSample()

Máquina de estados de detección. **Silenciosa** en v4 (sin impresiones).

```cpp
void processSample() {
  if (!setpointReady) {
    // Adquirir setpoint: promediar 20 lecturas
    accSumX += gx; accSumY += gy; accSumZ += gz;
    accCount++;
    if (accCount >= SETPOINT_N) {
      spX = accSumX / SETPOINT_N;
      spY = accSumY / SETPOINT_N;
      spZ = accSumZ / SETPOINT_N;
      setpointReady = true;
      // reset acumuladores
    }
    return;
  }

  // Desviación solo eje Z
  devZ = gz - spZ;
  bool eventActive = fabsf(devZ) > ARROW_THRESHOLD_G;  // 0.20 g
  bool allResting  = (fabsf(devZ) <= RETURN_G);        // 0.15 g

  switch (detectState) {
    case ST_IDLE:
      if (eventActive) {
        // Clasificar severidad
        eventChar = (devZ >=  SEVERE_THRESHOLD_G) ? '*'
                  : (devZ >   ARROW_THRESHOLD_G)  ? '+'
                  : (devZ <= -SEVERE_THRESHOLD_G) ? '!'
                  :                                '-';
        symbolPending = true;
        detectState = ST_LOCKED;
        lockUntilMs = millis() + EVENT_LOCKOUT_MS;  // 250 ms
        restSinceMs = 0;
        beginCapture();
      } else {
        // Recalcular setpoint en reposo
        accSumX += gx; accSumY += gy; accSumZ += gz;
        accCount++;
        if (accCount >= SETPOINT_N) { /* actualizar spX, spY, spZ */ }
      }
      break;

    case ST_LOCKED:
      // Desbloqueo: 3 condiciones
      if (!collecting && allResting) {
        if (restSinceMs == 0) restSinceMs = millis();
        if (millis() >= lockUntilMs &&
            (millis() - restSinceMs) >= REST_MS) {  // 50 ms
          detectState = ST_IDLE;
          restSinceMs = 0;
        }
      } else {
        restSinceMs = 0;
      }
      break;
  }
}
```

---

## 5. Funciones de red

### 5.1 startWifi()

Inicia la conexión WiFi STA de forma no bloqueante.

```cpp
void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiConnectRetries = 0;
  netStepMs = millis();
  netState = NET_WIFI_CONNECTING;
}
```

### 5.2 serviceWifi()

Sondea la conexión WiFi cada 250 ms. Si no hay enlace tras `MAX_WIFI_RETRIES`
(120 × 250 ms = 30 s), reinicia con MAC aleatoria.

### 5.3 startMqtt()

Configura el cliente PubSubClient y realiza el primer intento de conexión.

```cpp
void startMqtt() {
  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setBufferSize(MQTT_BUF_MAX);  // 384 B
  client.setCallback(mqttCallback);
  mqttConnectStart = millis();
  netStepMs = millis();
  netState = NET_MQTT_CONNECTING;
  mqttAttempt();  // primer intento inmediato
}
```

### 5.4 serviceMqtt()

Reintenta la conexión MQTT cada 1 s. Si no conecta dentro de
`MQTT_CONNECT_TIMEOUT` (120 s), reinicia con MAC aleatoria.

### 5.5 mqttCallback()

Procesa los mensajes entrantes. Solo implementa el comando `RECAL`.

```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  unsigned int n = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  if (strcmp(msg, "RECAL") == 0) {
    setpointReady = false;
    accSumX = accSumY = accSumZ = 0.0f;
    accCount = 0;
  }
}
```

### 5.6 sendToBroker()

Publica un mensaje en el broker MQTT.

```cpp
void sendToBroker(const char* topic, const char* message) {
  if (client.connected()) {
    char t[160];
    snprintf(t, sizeof(t), "%s/%s", MQTT_CLIENT, topic);
    if (client.publish(t, message)) {
      mqttPublishFailCount = 0;
      lastGoodPublish = millis();
    } else {
      mqttPublishFailCount++;
      if (mqttPublishFailCount >= MAX_MQTT_FAILURES) {
        rebootWithNewMac();
      }
    }
  } else {
    mqttPublishFailCount++;
    if (mqttPublishFailCount >= MAX_MQTT_FAILURES) {
      rebootWithNewMac();
    }
  }
}
```

### 5.7 Watchdog de ping

El firmware realiza pings ICMP a 8.8.8.8 cada 30 s. Si tras 3 rondas
consecutivas no hay respuesta de ping y el broker no es alcanzable, la placa
se reinicia con MAC aleatoria.

```cpp
void servicePing() {
  // Si hay ronda activa, esperar a que termine
  // Si pingReplies > 0 → OK, resetear contador de fallos
  // Si no hay ping pero brokerReachable() → OK
  // Si no hay ping ni broker → pingFailRounds++
  // Si pingFailRounds >= PING_FAIL_ROUNDS → rebootWithNewMac()
}
```

---

## 6. Contador de eventos persistente (RTC)

El contador de eventos se almacena en la RTC user memory del ESP8266:

| Palabras | Contenido |
|---|---|
| 0–1 | `rtcData_t` (magic + failureFlag) |
| 2 | Contador de eventos (`eventCount`) |
| 3 | Magic del contador (`0x45565431` = `'EVT1'`) |

```cpp
#define RTC_EVT_MAGIC 0x45565431UL        // 'E','V','T','1'
#define RTC_EVT_MAX   100000000UL         // por encima, es basura seguro

void loadEventCounter() {
  uint32_t v = 0, magic = 0;
  ESP.rtcUserMemoryRead(2, &v, sizeof(v));
  ESP.rtcUserMemoryRead(3, &magic, sizeof(magic));
  if (magic == RTC_EVT_MAGIC && v <= RTC_EVT_MAX && v != 0) {
    eventCount = v;
  } else {
    eventCount = 0;
    saveEventCounter();
  }
}

void saveEventCounter() {
  ESP.rtcUserMemoryWrite(2, &eventCount, sizeof(eventCount));
  uint32_t magic = RTC_EVT_MAGIC;
  ESP.rtcUserMemoryWrite(3, &magic, sizeof(magic));
}
```

**Nota**: la RTC user memory sobrevive a reinicios por software (RTC
alimentado) pero se pierde si se corta la luz.

---

## 7. Guía de modificación y extensión

### 7.1 Cambiar el umbral de publicación MQTT

Editar la constante en el `.ino`:

```cpp
#define PUBLISH_MIN_PEAK_G   0.58f
```

### 7.2 Cambiar el umbral de detección

```cpp
#define ARROW_THRESHOLD_G    0.20f   // detección de evento
#define SEVERE_FACTOR        1.2f    // severo = ARROW * SEVERE_FACTOR
#define RETURN_G             0.15f   // histéresis de retorno
```

### 7.3 Cambiar la ventana de captura

```cpp
#define PRE_N                100     // muestras previas (~1 s a 100 Hz)
#define POST_N               100     // muestras posteriores (~1 s a 100 Hz)
```

### 7.4 Agregar nuevos comandos MQTT

Editar `mqttCallback()`:

```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  unsigned int n = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  if (strcmp(msg, "RECAL") == 0) {
    // Recalibrar setpoint
    setpointReady = false;
    accSumX = accSumY = accSumZ = 0.0f;
    accCount = 0;
  }
  // Agregar nuevos comandos aquí:
  // else if (strcmp(msg, "NUEVO_CMD") == 0) { ... }
}
```

### 7.5 Agregar nuevos topics de publicación

Editar `publishPeakEvent()` o crear nuevas funciones de publicación:

```cpp
void publishNewData() {
  char msg[64];
  snprintf(msg, sizeof(msg), "%.2f", someValue);
  sendToBroker("report/newdata", msg);
}
```

### 7.6 Cambiar el intervalo de heartbeat

```cpp
#define HEARTBEAT_INTERVAL_MS 30000UL    // 30 s
```

### 7.7 Cambiar el timeout de stale publish

```cpp
#define STALE_REPORT_MS   900000UL       // 15 min
```

---

## 8. API de funciones

### 8.1 Funciones públicas (llamadas desde setup/loop)

| Función | Propósito |
|---|---|
| `setup()` | Inicialización del sistema |
| `loop()` | Bucle principal |

### 8.2 Funciones de sensor

| Función | Propósito |
|---|---|
| `readAccel()` | Lee el ADXL345 y convierte a g |
| `readRawAxes()` | Lee los 6 bytes crudos del ADXL345 |
| `readReg(reg)` | Lee un registro del ADXL345 |
| `writeReg(reg, val)` | Escribe un registro del ADXL345 |
| `runChipDiagnostic()` | Fuerza measurement mode (silencioso) |

### 8.3 Funciones de detección

| Función | Propósito |
|---|---|
| `processSample()` | Máquina de estados IDLE/LOCKED |
| `pushSample(dt)` | Escribe en el anillo del pasado |
| `beginCapture()` | Abre la ventana de captura |
| `advanceCapture(dt)` | Actualiza el pico en línea |
| `finishCapture()` | Cierra la ventana y genera salida |
| `publishPeakEvent()` | Publica el pico por MQTT |

### 8.4 Funciones de red

| Función | Propósito |
|---|---|
| `startWifi()` | Inicia conexión WiFi |
| `serviceWifi()` | Sondea conexión WiFi |
| `startMqtt()` | Inicia conexión MQTT |
| `serviceMqtt()` | Sondea conexión MQTT |
| `mqttAttempt()` | Intento de conexión MQTT |
| `mqttCallback()` | Procesa mensajes entrantes |
| `sendToBroker(topic, msg)` | Publica en el broker |
| `servicePing()` | Gestiona rondas de ping |
| `startPingRound()` | Inicia una ronda de ping |
| `brokerReachable()` | Verifica si el broker responde |
| `randomizeMac()` | Cambia la MAC aleatoriamente |
| `rebootWithNewMac()` | Reinicia con MAC nueva |

### 8.5 Funciones de persistencia

| Función | Propósito |
|---|---|
| `loadEventCounter()` | Carga el contador de eventos desde RTC |
| `saveEventCounter()` | Guarda el contador de eventos en RTC |

### 8.6 Funciones de salida

| Función | Propósito |
|---|---|
| `printCharTick()` | Imprime línea CSV (solo MODE 1) |
| `blinkLedDummy()` | Función vacía (placeholder) |

---

## 9. Diagrama de flujo de la lógica principal

```
┌─────────────────────────────────────────────────────────────────┐
│                         setup()                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 1. WDT enable                                             │   │
│  │ 2. Serial begin (silencioso)                              │   │
│  │ 3. Load event counter (RTC)                               │   │
│  │ 4. Check failure flag → randomizeMac()                    │   │
│  │ 5. Wire.begin(4,5)                                        │   │
│  │ 6. accel.begin() → setRange(±2g) → setDataRate(100Hz)   │   │
│  │ 7. runChipDiagnostic() (silencioso)                       │   │
│  │ 8. startWifi()                                            │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                          loop()                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 1. MUESTREO PRIMERO (~100 Hz) — tiene prioridad sobre red │   │
│  │    ┌────────────────────────────────────────────────┐     │   │
│  │    │ readAccel() → pushSample() → processSample()   │     │   │
│  │    │                  → advanceCapture()             │     │   │
│  │    └────────────────────────────────────────────────┘     │   │
│  │                                                            │   │
│  │ 2. Reintento sensor (si no responde)                       │   │
│  │                                                            │   │
│  │ 3. RED DESPUÉS (solo MODE 0 && !MQTT_DRY_RUN) — no bloqueante│
│  │    ┌────────────────────────────────────────────────┐     │   │
│  │    │ serviceWifi() → serviceMqtt() → client.loop()  │     │   │
│  │    │ → servicePing() → heartbeat → stale check       │     │   │
│  │    └────────────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    processSample()                               │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ if (!setpointReady)                                        │   │
│  │   → Acquirir setpoint (20 lecturas)                        │   │
│  │   → return                                                 │   │
│  │                                                            │   │
│  │ devZ = gz - spZ                                            │   │
│  │                                                            │   │
│  │ switch (detectState)                                       │   │
│  │   case ST_IDLE:                                            │   │
│  │     if (|devZ| > 0.20g)                                    │   │
│  │       → Clasificar (*, +, !, -)                            │   │
│  │       → ST_LOCKED                                          │   │
│  │       → beginCapture()                                     │   │
│  │     else                                                   │   │
│  │       → Recalcular setpoint (cada 20 muestras)             │   │
│  │                                                            │   │
│  │   case ST_LOCKED:                                          │   │
│  │     if (!collecting && |devZ| <= 0.15g)                    │   │
│  │       → Esperar lockout (250ms) + reposo (50ms)            │   │
│  │       → ST_IDLE                                           │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    finishCapture()                               │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ if (eventPeakZ > 0.58g)                                    │   │
│  │   → publishPeakEvent() → MQTT report/value                 │   │
│  │                                                            │   │
│  │ Serial.printf("%.2f\n", eventPeakZ)                        │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10. Notas de implementación

### 10.1 Orden crítico de llamadas

El orden `pushSample() → processSample() → advanceCapture()` es **crítico**:

1. `pushSample()` debe ir antes de `processSample()` para que la muestra que
   dispara el evento ya esté en el anillo.
2. `processSample()` debe ir antes de `advanceCapture()` para que `devZ`
   esté fresco al medir el pico.

### 10.2 Sin delay() en el loop

El loop no usa `delay()`. Todas las esperas son no bloqueantes basadas en
`millis()`. Esto es esencial para mantener el muestreo a ~100 Hz.

### 10.3 Watchdog

El watchdog de hardware (`WDTO_8S`) se alimenta con `ESP.wdtFeed()` en todos
los bucles de espera. Si el loop se bloquea más de 8 s, la placa se reinicia.

### 10.4 Reconexión de red no bloqueante

Las reconexiones WiFi/MQTT son no bloqueantes. No se pierden golpes durante
una reconexión porque el muestreo tiene prioridad sobre la red.

### 10.5 Contador RTC

El contador de eventos se guarda en RTC user memory. Sobrevive a reinicios
por software pero se pierde si se corta la luz. La validación incluye:
- Magic correcto (`'EVT1'`)
- Valor dentro de rango (`≤ RTC_EVT_MAX`)
- Valor no cero

---

## 11. Compilación y carga

```bash
# Compilar
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 .

# Detectar puerto
arduino-cli board list

# Cargar
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 .
```

---

## 12. Estructura de archivos

```
BusAccel-8266-v4/
├── BusAccel-8266-v4.ino      # Sketch principal
├── secrets.h                 # Credenciales reales (NO versionado)
├── secrets.example.h         # Plantilla de credenciales
├── .gitignore                # Exclusiones
├── PLAN.md                   # Plan de implementación
├── AUDIT_REPORT.md           # Informe de auditoría
├── README.md                 # Documentación general
└── docs/
    ├── datasheet.md          # Documentación de usuario
    └── developer.md          # Este archivo
```
