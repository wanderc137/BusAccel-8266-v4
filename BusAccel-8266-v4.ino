// ============================================================================
// BusAccel-8266-v4 — PICO DEL EVENTO (eje Z) CON PUBLICACION MQTT CONDICIONAL
// Placa: NodeMCU ESP8266 (ESP-12E)  |  FQBN: esp8266:esp8266:nodemcuv2
//
// Sensor: GY-291 (ADXL345) via I2C (rango +-2g, 256 LSB/g = raw/256.0f)
//   SDA -> D2/GPIO4 , SCL -> D1/GPIO5  (Wire.begin(4,5))
//
// QUE CAMBIA RESPECTO DE v3 (BusAccel-8266-v3):
//   1. PUBLICACION MQTT CONDICIONAL: nueva constante PUBLISH_MIN_PEAK_G = 0.58f.
//      Solo se publica por MQTT si el pico del evento es ESTRICTAMENTE MAYOR que
//      0.58 g. Si es <= 0.58 g, NO se publica nada por MQTT (pero el evento SI
//      se imprime por serie).
//   2. ARRANQUE 100% SILENCIOSO: se eliminan TODAS las impresiones de arranque y
//      diagnostico por serie (banner, [FW], [RTC], diagnostico profundo ADXL345,
//      umbrales, broker, puntos de adquisicion de setpoint, "setpoint
//      establecido", eco [MQTT-OUT]). El puerto serie queda en silencio total
//      salvo las lineas de evento.
//   3. SALIDA SERIE DE EVENTOS: UNA sola linea por evento con SOLO el numero del
//      pico, formato "%.2f\n" (ej. "0.44"), sin prefijo [PICO] ni "g". Se imprime
//      para TODOS los eventos detectados (umbral 0.20 g), sea o no > 0.58 g.
//   4. MQTT_DRY_RUN por DEFECTO = 0 (MQTT REAL con WiFi STA y broker).
//
// SE MANTIENE DE v3 (arquitectura intacta):
//   - Deteccion SOLO sobre eje Z (devZ = gz - spZ), setpoint de 20 lecturas,
//     recalculado cada 20 muestras en reposo.
//   - Simbolos '.', '+', '*', '-', '!' y umbrales 0.20 g / severo 0.24 g /
//     retorno 0.15 g.
//   - Maquina de estados ST_IDLE/ST_LOCKED con EVENT_LOCKOUT_MS + REST_MS y la
//     tercera condicion de desbloqueo !collecting.
//   - Muestreo ~100 Hz sin delay() en el loop; orden pushSample -> processSample
//     -> advanceCapture.
//   - Modos MODE_SERIAL_CSV 0/1.
//   - Red: WiFi STA con reintentos, PubSubClient, ping ICMP a 8.8.8.8 con
//     fallback brokerReachable(), reinicio con MAC aleatoria, WDTO_8S.
//   - Forzado de measurement mode (diagnostico SILENCIOSO).
//   - Reintento del sensor en background si no aparece en I2C.
//   - Contador de evento persistente en RTC user memory (palabras 2-3, magic 'EVT1'),
//     con validacion reforzada (magic + v<=RTC_EVT_MAX + v!=0) y sizeof(...).
//   - Suscripcion a <MQTT_CLIENT>/#; payload RECAL recalibra el setpoint.
//   - Credenciales en secrets.h (no versionado); secrets.example.h de plantilla.
//   - Calculo del pico con beginCapture() (escaneo del anillo previo) y
//     advanceCapture() (actualizacion en linea con cada muestra POST).
//
// MODOS (definidos arriba del todo):
//   MODE_SERIAL_CSV 0  -> deteccion por evento + MQTT (modo normal)
//   MODE_SERIAL_CSV 1  -> CSV puro a 20 Hz, sin WiFi (herramienta de v1/v2)
//   MQTT_DRY_RUN   0   -> MQTT REAL (por defecto en v4)
//   MQTT_DRY_RUN   1   -> NO se abre WiFi ni MQTT; los envios son silenciosos
//
// CONTRATO DE DATOS (v4, convencion SmartAge `report/`):
//   <MQTT_CLIENT>/report/online      "true"   (heartbeat: al conectar y luego cada ~30 s)
//   <MQTT_CLIENT>/report/value       "0.62"   (texto plano, solo si peak > 0.58 g)
//   <MQTT_CLIENT>/directive/<cmd>    (comandos web; se conserva RECAL)
//   (sin setpoint ni status: arranque y runtime silenciosos salvo heartbeat + golpes)
//
// NOTA SOBRE EL TIEMPO: no hay NTP/RTC de hora. El pico se reporta en g.
//
// MAPA DE PINES:
//   D1/GPIO5  SCL -> GY-291 SCL
//   D2/GPIO4  SDA -> GY-291 SDA
//   3V3/GND   alimentacion GY-291 (NUNCA 5 V)
//   TX/GPIO1  UART -> monitor serie / app (115200 baud)
// ============================================================================

// 0 = MQTT por evento (modo normal)   1 = solo CSV por serie, SIN WiFi
#define MODE_SERIAL_CSV  0

// Ensayo en seco: 1 = NO se abre WiFi ni MQTT (envios silenciosos).
// 0 = MQTT REAL. En v4 el valor por defecto es 0.
#define MQTT_DRY_RUN  0

#include <Wire.h>
#include <math.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>

#if MODE_SERIAL_CSV == 0
  #include <ESP8266WiFi.h>
  #include <PubSubClient.h>
  extern "C" {
    #include <user_interface.h>
    #include <ping.h>
  }
  #include "secrets.h"
#endif

// ---------------------------------------------------------------------------
// Constantes
// ---------------------------------------------------------------------------
#define BAUD_RATE            115200
#define FW_VERSION          "4"
#define FW_BUILD            "2026-10-08"
#define SENSOR_RANGE         ADXL345_RANGE_2_G
#define SENSOR_DATARATE      ADXL345_DATARATE_100_HZ
#define SAMPLE_INTERVAL_MS   10        // muestreo interno ~100 Hz
#define PRINT_INTERVAL_MS    50        // impresion serial ~20 Hz (modo serie puro)
#define SENSOR_RETRY_MS      1000      // reintento en background cada 1 s

// Setpoint
#define SETPOINT_N           20            // lecturas para el promedio por eje
#define ARROW_THRESHOLD_G    0.20f         // desviacion Z (g) para '+'/'-'
#define SEVERE_FACTOR        1.2f
#define SEVERE_THRESHOLD_G   (ARROW_THRESHOLD_G * SEVERE_FACTOR)  // 0.24g
#define RETURN_G             0.15f         // histeresis de retorno a reposo

// v4: umbral de PUBLICACION MQTT (el pico debe superarlo ESTRICTAMENTE para
// que el evento se publique). La impresion serie NO depende de este umbral.
#define PUBLISH_MIN_PEAK_G   0.58f

// --- Anulacion del rebote (identico a v1/v2) ---
#define EVENT_LOCKOUT_MS     250
#define REST_MS              50

// --- Ventana del evento ---
// PRE_N muestras se mantienen SIEMPRE en un anillo: es lo que permite medir el
// pico de lo que ocurrio ANTES del cruce. La ventana no se almacena completa:
// el pico se calcula en linea.
#define PRE_N                100           // 1 s a 100 Hz
#define POST_N               100           // 1 s a 100 Hz
#define EVENT_N              (PRE_N + POST_N)          // 200
#define EVENT_SPAN_MS        ((PRE_N + POST_N) * SAMPLE_INTERVAL_MS)  // 2000 ms

// ---------------------------------------------------------------------------
// Constantes de red (solo modo MQTT)
// ---------------------------------------------------------------------------
#define MAX_WIFI_RETRIES    120          // 120 x 250 ms = 30 s
#define MAX_MQTT_FAILURES    10
#define MQTT_CONNECT_TIMEOUT 120000UL
#define MQTT_BUF_MAX         384         // payload de pico ~150 B
#define WDT_FEED_INTERVAL    4           // s (margen 2x sobre WDTO_8S)
#define STALE_REPORT_MS   900000UL       // 15 min sin publicar nada -> reinicio
#define HEARTBEAT_INTERVAL_MS 30000UL    // heartbeat report/online cada ~30 s

// Conectividad ICMP a 8.8.8.8
#define PING_TARGET_IP     0x08080808UL
#define PING_MAX_TRIES         5
#define PING_TIMEOUT_MS   3000UL
#define PING_GAP_MS       2000UL
#define PING_ROUND_MAX_MS 20000UL
#define PING_INTERVAL        30
#define PING_FAIL_ROUNDS      3           // rondas seguidas sin ping Y sin broker -> reset

// ---------------------------------------------------------------------------
// Pines y registros ADXL345
// ---------------------------------------------------------------------------
#define PIN_SDA              4         // D2/GPIO4
#define PIN_SCL              5         // D1/GPIO5

#define ADXL345_I2C_ADDR     0x53
#define REG_DEVID            0x00
#define REG_OFSX             0x1E
#define REG_OFSY             0x1F
#define REG_OFSZ             0x20
#define REG_BW_RATE          0x2C
#define REG_POWER_CTL        0x2D
#define REG_DATA_FORMAT      0x31
#define REG_DATAX0           0x32
#define REG_DATAY0           0x34
#define REG_DATAZ0           0x36

// ---------------------------------------------------------------------------
// Estructura de una muestra del anillo
//   x, y, z en cuentas crudas del ADXL345 (+-2 g -> 256 LSB/g).
//   dt = ms REALES entre esta muestra y la anterior.
// ---------------------------------------------------------------------------
struct sample_t { int16_t x, y, z; uint16_t dt; };   // 8 bytes

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
Adafruit_ADXL345_Unified accel(12345);

bool sensorOnline = false;
bool notDetectedReported = false;
float gx = 0.0f, gy = 0.0f, gz = 0.0f;
int16_t rawX = 0, rawY = 0, rawZ = 0;

bool setpointReady = false;
float spX = 0.0f, spY = 0.0f, spZ = 0.0f;
float devZ = 0.0f;                            // solo el eje Z decide
float accSumX = 0.0f, accSumY = 0.0f, accSumZ = 0.0f;
uint16_t accCount = 0;

enum DetectState : uint8_t { ST_IDLE = 0, ST_LOCKED = 1 };
DetectState detectState = ST_IDLE;
char  eventChar = '.';
bool  symbolPending = false;

unsigned long lockUntilMs = 0;
unsigned long restSinceMs = 0;

// --- Ventana del evento (pico en linea, sin eventBuf) ---
sample_t preRing[PRE_N];       // anillo del pasado, siempre rodando
uint8_t  ringIdx    = 0;       // proxima escritura en preRing
uint16_t postCount  = 0;       // muestras POST ya contadas
bool     collecting = false;   // hay una ventana en curso
uint32_t eventStartMs = 0;
float    eventDevZ   = 0.0f;   // desviacion Z en el instante del cruce
float    eventPeakZ  = 0.0f;   // max |devZ| de la ventana (pre + post)
char     eventSym    = '.';
uint32_t eventCount  = 0;      // eventos detectados desde el arranque

unsigned long lastSampleMs = 0;
unsigned long lastPrintMs = 0;
unsigned long lastRetryMs = 0;

// ---------------------------------------------------------------------------
// Estado de red (solo modo MQTT)
// ---------------------------------------------------------------------------
#if MODE_SERIAL_CSV == 0
WiFiClient espClient;
PubSubClient client(espClient);
// I-002: sin buffer global mqttBuf[MQTT_BUF_MAX]; el propio PubSubClient
// reserva su buffer con client.setBufferSize(MQTT_BUF_MAX).

int mqttPublishFailCount = 0;
int wifiConnectRetries = 0;

// Maquinas de estado NO bloqueantes de red (WiFi/MQTT). Evitan que un reset
// del ESP8266 en mitad de una transaccion I2C o una reconexion larga bloqueen
// el loop() (y con el, el muestreo a ~100 Hz).
enum : uint8_t {
  NET_IDLE = 0,
  NET_WIFI_CONNECTING,
  NET_WIFI_READY,
  NET_MQTT_CONNECTING,
  NET_MQTT_READY
};
uint8_t  netState = NET_IDLE;
uint32_t netStepMs = 0;          // temporizador de sondeo (250 ms WiFi / 1 s MQTT)
uint32_t mqttConnectStart = 0;   // inicio de la ventana de conexion MQTT

typedef struct {
  uint32_t magic;
  uint16_t failureFlag;
} rtcData_t;

volatile bool    pingRoundActive = false;
volatile bool    pingRoundDone = false;
volatile uint32_t pingReplies = 0;
ping_resp        pingLast;
struct ping_option pingOpt;
uint32_t pingNextTryAt = 0;
uint32_t pingRoundStart = 0;
uint32_t lastGoodPublish = 0;
uint8_t  pingFailRounds = 0;   // rondas seguidas sin ping Y sin broker

#if MQTT_DRY_RUN
uint8_t  dryMsgs  = 0;      // mensajes "publicados" en el ensayo
uint32_t dryBytes = 0;      // bytes acumulados del ensayo
#else
unsigned long lastStaleCheckMs = 0;   // I-003: ultima comprobacion de stale publish
unsigned long lastHeartbeatMs  = 0;   // ultimo envio de report/online
#endif
#endif

// ---------------------------------------------------------------------------
// Declaraciones
// ---------------------------------------------------------------------------
void blinkLedDummy();
bool readAccel();
void processSample();
void printCharTick();
void runChipDiagnostic();
uint8_t readReg(uint8_t reg);
bool writeReg(uint8_t reg, uint8_t val);
bool readRawAxes();
void pushSample(uint16_t dt);
void beginCapture();
void advanceCapture(uint16_t dt);
void finishCapture();
void publishPeakEvent();
void loadEventCounter();
void saveEventCounter();

#if MODE_SERIAL_CSV == 0
void startWifi();
void serviceWifi();
void startMqtt();
void serviceMqtt();
void mqttAttempt();
void sendToBroker(const char* topic, const char* message);
void randomizeMac();
void rebootWithNewMac();
void servicePing();
void startPingRound();
bool brokerReachable();
void onPingRecv(void* arg, void* pdata);
void onPingSent(void* arg, void* pdata);
void mqttCallback(char* topic, byte* payload, unsigned int length);
#endif

// ---------------------------------------------------------------------------
// Acceso directo a registros del ADXL345 por I2C (Wire)
// ---------------------------------------------------------------------------
uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(ADXL345_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)ADXL345_I2C_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADXL345_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return (Wire.endTransmission(true) == 0);
}

bool readRawAxes() {
  Wire.beginTransmission(ADXL345_I2C_ADDR);
  Wire.write(REG_DATAX0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADXL345_I2C_ADDR, (uint8_t)6) != 6) return false;
  uint8_t x0 = Wire.read(), x1 = Wire.read();
  uint8_t y0 = Wire.read(), y1 = Wire.read();
  uint8_t z0 = Wire.read(), z1 = Wire.read();
  rawX = (int16_t)((uint16_t)x1 << 8 | x0);
  rawY = (int16_t)((uint16_t)y1 << 8 | y0);
  rawZ = (int16_t)((uint16_t)z1 << 8 | z0);
  return true;
}

// NOTA (v4): se ELIMINO i2cBusRecover(). En banco se comprobo que esa
// secuencia de pulsos manuales sobre SCL/SDA dejaba al ADXL345 sin responder
// (WHO_AM_I=NO_ACK) pese a que el bus estaba sano. Sin ella, el sensor
// responde siempre (0x53, WHO_AM_I=0xE5). NO reintroducir.

// ---------------------------------------------------------------------------
// Diagnostico del chip (v4: SILENCIOSO). Se conserva la operacion necesaria
// (forzar measurement mode), pero NO se imprime nada por serie.
// ---------------------------------------------------------------------------
void runChipDiagnostic() {
  // Forzado de measurement mode: escribiendo 0x08 en POWER_CTL (bit3=1).
  writeReg(REG_POWER_CTL, 0x08);
  delay(50);
  // Lectura de descarte para asegurar datos frescos (sin impresion).
  readRawAxes();
}

// ---------------------------------------------------------------------------
// Lectura del acelerometro: burst de 6 bytes -> g = raw / 256.0f (+-2g)
// ---------------------------------------------------------------------------
bool readAccel() {
  if (readRawAxes()) {
    sensorOnline = true;
    gx = rawX / 256.0f;
    gy = rawY / 256.0f;
    gz = rawZ / 256.0f;
    return true;
  }
  sensorOnline = false;
  return false;
}

// ---------------------------------------------------------------------------
// Contador de evento persistente en RTC user memory.
//   palabras 0-1 -> rtcData_t (magic + failureFlag)
//   palabra  2   -> contador de evento
//   palabra  3   -> magic del contador ("EVT1")
// No toca la flash: sobrevive a reinicios por red (RTC alimentado) pero se
// pierde si se corta la luz. Validacion reforzada.
// ---------------------------------------------------------------------------
#define RTC_EVT_MAGIC 0x45565431UL        // 'E','V','T','1'
#define RTC_EVT_MAX   100000000UL         // por encima, es basura seguro

void loadEventCounter() {
  uint32_t v = 0, magic = 0;
  // I-001: la API espera el TAMANIO EN BYTES, no un numero de palabras.
  ESP.rtcUserMemoryRead(2, &v, sizeof(v));
  ESP.rtcUserMemoryRead(3, &magic, sizeof(magic));
  // magic correcto Y dentro de rango Y no es la palabra 2 sin inicializar (0)
  if (magic == RTC_EVT_MAGIC && v <= RTC_EVT_MAX && v != 0) {
    eventCount = v;
  } else {
    eventCount = 0;
    saveEventCounter();
    // v4: sin impresion (arranque silencioso)
  }
}

void saveEventCounter() {
  // I-001: tamano en BYTES (4 para el contador y su magic).
  ESP.rtcUserMemoryWrite(2, &eventCount, sizeof(eventCount));
  uint32_t magic = RTC_EVT_MAGIC;
  ESP.rtcUserMemoryWrite(3, &magic, sizeof(magic));
}

// ---------------------------------------------------------------------------
// Anillo del pasado. Se escribe en CADA muestra, evento o no: es lo que
// permite medir el pico de lo ocurrido antes del cruce.
// ---------------------------------------------------------------------------
void pushSample(uint16_t dt) {
  preRing[ringIdx].x  = rawX;
  preRing[ringIdx].y  = rawY;
  preRing[ringIdx].z  = rawZ;
  preRing[ringIdx].dt = dt;
  ringIdx = (uint8_t)((ringIdx + 1) % PRE_N);
}

// ---------------------------------------------------------------------------
// Arranque de la ventana: se escanea el anillo del pasado para medir el pico
// inicial, en vez de copiarlo (no se almacena la ventana completa).
// ---------------------------------------------------------------------------
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
  saveEventCounter();          // consume id al abrir (ids sin huecos)
}

// ---------------------------------------------------------------------------
// Avance de la ventana: actualiza el pico con cada muestra POST y cuenta. Al
// llegar a POST_N cierra la ventana. No almacena muestras.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Cierre de la ventana (v4): MQTT condicional + UNA linea serie con el pico.
// ---------------------------------------------------------------------------
void finishCapture() {
#if MODE_SERIAL_CSV == 0
  // 1. Publicacion MQTT CONDICIONAL: solo si el pico supera el umbral.
  if (eventPeakZ > PUBLISH_MIN_PEAK_G) {
    publishPeakEvent();
  }
  // 2. Salida serie: UNA sola linea, SOLO el numero del pico (%.2f).
  //    Se imprime SIEMPRE (evento detectado), independiente del umbral MQTT.
  Serial.printf("%.2f\n", eventPeakZ);
#endif
}

void blinkLedDummy() {}

// ---------------------------------------------------------------------------
// Publicacion MQTT del pico: UN solo mensaje por evento (topic nuevo).
// v4: solo se invoca cuando eventPeakZ > PUBLISH_MIN_PEAK_G.
// ---------------------------------------------------------------------------
#if MODE_SERIAL_CSV == 0
void publishPeakEvent() {
  char msg[16];
  snprintf(msg, sizeof(msg), "%.2f", eventPeakZ);

#if MQTT_DRY_RUN
  (void)msg;
#else
  if (!client.connected()) {
    return;
  }
  sendToBroker("report/value", msg);
#endif
}
#endif

// ---------------------------------------------------------------------------
// Procesado de una muestra (~100 Hz). Logica identica a v3 (solo Z decide),
// pero v4 es SILENCIOSA (sin puntos de adquisicion ni "setpoint establecido").
// ---------------------------------------------------------------------------
void processSample() {
  if (!setpointReady) {
    accSumX += gx; accSumY += gy; accSumZ += gz;
    accCount++;
    // v4: SIN Serial.print('.')
    if (accCount >= SETPOINT_N) {
      spX = accSumX / SETPOINT_N;
      spY = accSumY / SETPOINT_N;
      spZ = accSumZ / SETPOINT_N;
      setpointReady = true;
      accSumX = accSumY = accSumZ = 0.0f;
      accCount = 0;
      // v4: SIN Serial.println() ni "setpoint establecido..."
      // v5: SIN publicacion de setpoint (silencio; solo heartbeat + golpes)
    }
    return;
  }

  // La desviacion que decide es SOLO la de Z (gx, gy y spX/spY se siguen
  // calculando para la linea CSV y para el setpoint, pero no disparan evento).
  devZ        = gz - spZ;
  bool eventActive = fabsf(devZ) > ARROW_THRESHOLD_G;
  bool allResting  = (fabsf(devZ) <= RETURN_G);

  switch (detectState) {

    case ST_IDLE:
      if (eventActive) {
        // Sentido y magnitud de la desviacion de Z respecto del setpoint:
        //   devZ >=  +SEVERE  -> '*'   elevacion severa
        //   devZ >   +ARROW   -> '+'   elevacion leve
        //   devZ <= -SEVERE   -> '!'   caida severa
        //   devZ <   -ARROW   -> '-'   caida leve
        eventChar = (devZ >=  SEVERE_THRESHOLD_G) ? '*'
                  : (devZ >   ARROW_THRESHOLD_G)  ? '+'
                  : (devZ <= -SEVERE_THRESHOLD_G) ? '!'
                  :                                '-';
        symbolPending = true;
        detectState = ST_LOCKED;
        lockUntilMs = millis() + EVENT_LOCKOUT_MS;
        restSinceMs = 0;
        beginCapture();          // arranca la ventana del evento
      } else {
        accSumX += gx; accSumY += gy; accSumZ += gz;
        accCount++;
        if (accCount >= SETPOINT_N) {
          spX = accSumX / SETPOINT_N;
          spY = accSumY / SETPOINT_N;
          spZ = accSumZ / SETPOINT_N;
          accSumX = accSumY = accSumZ = 0.0f;
          accCount = 0;
        }
      }
      break;

    case ST_LOCKED:
      // Sin simbolos adicionales (rebote). El desbloqueo exige LAS TRES
      // condiciones:
      //   (a) lockout temporal ya cumplido  -> anula el rebote ~100-200 ms
      //   (b) reposo sostenido REST_MS      -> no un instante aislado
      //   (c) ventana del evento TERMINADA  -> sin esto, un evento a los 300 ms
      //       podria pisar la ventana mientras la anterior sigue llenandose.
      if (!collecting && allResting) {
        if (restSinceMs == 0) restSinceMs = millis();
        if (millis() >= lockUntilMs &&
            (millis() - restSinceMs) >= REST_MS) {
          detectState = ST_IDLE;
          restSinceMs = 0;
        }
      } else {
        restSinceMs = 0;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Linea CSV por tick a 20 Hz (solo modo MODE_SERIAL_CSV == 1)
//   t,X,Y,Z,setX,setY,setZ,sym  -- formato IDENTICO a v1/v2
// ---------------------------------------------------------------------------
void printCharTick() {
  char c = '.';
  if (symbolPending) {
    c = eventChar;
    symbolPending = false;
  }
  Serial.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%c\n",
                millis(), gx, gy, gz, spX, spY, spZ, c);
}

// ═══════════════════════════════════════════════════════════════════════════
//  RED (solo modo MQTT)  — v4: SILENCIOSA (arranque y runtime sin impresiones)
// ═══════════════════════════════════════════════════════════════════════════
#if MODE_SERIAL_CSV == 0

void randomizeMac() {
  WiFi.disconnect();
  uint8_t mac[6]; WiFi.macAddress(mac);
  mac[5] = random(0x01, 0xFE);
  wifi_set_macaddr(STATION_IF, mac);
  // v4: sin impresion
}

void rebootWithNewMac() {
  rtcData_t rtc; rtc.magic = 0xDEAD; rtc.failureFlag = 1;
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtc, sizeof(rtc));  // I-001: 8 bytes
  delay(100); ESP.restart();
}

void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiConnectRetries = 0;
  netStepMs = millis();
  netState = NET_WIFI_CONNECTING;
  // v4: sin impresion de conexion
}

// Paso NO bloqueante de WiFi: sondea cada 250 ms. Si no hay enlace tras
// MAX_WIFI_RETRIES sondeos (=30 s) reinicia con MAC aleatoria. No usa delay()
// largos ni while().
void serviceWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (netState == NET_WIFI_CONNECTING) {
      wifiConnectRetries = 0;
      netState = NET_WIFI_READY;
      if (!client.connected()) startMqtt(); else netState = NET_MQTT_READY;
    } else if (netState == NET_IDLE) {
      netState = NET_WIFI_READY;
      if (!client.connected()) startMqtt(); else netState = NET_MQTT_READY;
    }
    return;
  }
  // Sin enlace: iniciar (o reiniciar) la conexion y contabilizar el timeout.
  if (netState != NET_WIFI_CONNECTING) { startWifi(); return; }
  uint32_t now = millis();
  if (now - netStepMs < 250UL) return;
  netStepMs = now;
  if (++wifiConnectRetries >= MAX_WIFI_RETRIES) rebootWithNewMac();
  ESP.wdtFeed();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  (void)topic;
  char msg[64];
  unsigned int n = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';
  // v4: sin impresion de eco RX

  // Recalibracion en caliente: util con el bus en marcha, para fijar el
  // setpoint en el angulo real de montaje sin reiniciar el equipo.
  if (strcmp(msg, "RECAL") == 0) {
    setpointReady = false;
    accSumX = accSumY = accSumZ = 0.0f;
    accCount = 0;
    // v4: sin impresion
  }
}

// Intento unico de conexion MQTT (PubSubClient conserva su timeout de socket
// interno). NO bloquea con while()/delay() largos desde el loop().
void mqttAttempt() {
  if (client.connect(MQTT_CLIENT, MQTT_USERNAME, MQTT_PASSWORD)) {
    mqttPublishFailCount = 0;
    lastGoodPublish = millis();
    char sub[128]; snprintf(sub, sizeof(sub), "%s/#", MQTT_CLIENT);
    client.subscribe(sub);
    sendToBroker("report/online", "true");
    lastHeartbeatMs = millis();   // no duplicar el heartbeat inmediato
    netState = NET_MQTT_READY;
  }
}

void startMqtt() {
  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setBufferSize(MQTT_BUF_MAX);
  client.setCallback(mqttCallback);
  mqttConnectStart = millis();
  netStepMs = millis();
  netState = NET_MQTT_CONNECTING;
  mqttAttempt();   // primer intento inmediato
}

// Paso NO bloqueante de MQTT: reintenta cada 1 s. Si no conecta dentro de
// MQTT_CONNECT_TIMEOUT (120 s) reinicia con MAC aleatoria.
void serviceMqtt() {
  if (netState == NET_MQTT_READY) {
    if (client.connected()) return;
    lastGoodPublish = millis();
    startMqtt();
    return;
  }
  if (netState != NET_MQTT_CONNECTING) return;
  if (client.connected()) { netState = NET_MQTT_READY; return; }
  uint32_t now = millis();
  if (now - mqttConnectStart > MQTT_CONNECT_TIMEOUT) rebootWithNewMac();
  if (now - netStepMs < 1000UL) return;   // un intento por segundo
  netStepMs = now;
  mqttAttempt();
  ESP.wdtFeed();
}

void sendToBroker(const char* topic, const char* message) {
#if MQTT_DRY_RUN
  // ---- ENSAYO EN SECO: nada sale de la placa, sin eco por serie ----
  dryMsgs++;
  dryBytes += strlen(message);
  lastGoodPublish = millis();
  mqttPublishFailCount = 0;
#else
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
#endif
}

// --- ICMP real a 8.8.8.8 ---
void onPingRecv(void* arg, void* pdata) {
  (void)arg;
  ping_resp* r = (ping_resp*)pdata;
  if (r == NULL) return;
  pingLast = *r;
  if (r->ping_err == 0) pingReplies++;
  if (r->total_count >= PING_MAX_TRIES || r->timeout_count >= PING_MAX_TRIES) {
    pingRoundDone = true;
  }
}

void onPingSent(void* arg, void* pdata) { (void)arg; (void)pdata; }

void startPingRound() {
  pingReplies = 0;
  pingRoundDone = false;
  memset(&pingLast, 0, sizeof(pingLast));

  pingOpt.count       = PING_MAX_TRIES;
  pingOpt.ip          = PING_TARGET_IP;
  pingOpt.coarse_time = PING_GAP_MS;
  pingOpt.recv_function = onPingRecv;
  pingOpt.sent_function = onPingSent;
  pingOpt.reverse     = NULL;

  if (ping_start(&pingOpt)) {
    pingRoundActive = true;
    pingRoundStart  = millis();
  }
  ESP.wdtFeed();
}

void servicePing() {
  uint32_t now = millis();
  uint32_t elapsed = now - pingRoundStart;

  if (pingRoundActive) {
    bool roundOver = pingRoundDone;
    if (!roundOver && elapsed >= PING_ROUND_MAX_MS) roundOver = true;
    if (!roundOver) return;

    pingRoundActive = false;

    // Ronda cerrada -> evaluar conectividad global.
    if (pingReplies > 0) {
      // Hubo respuesta ICMP: todo OK.
      pingFailRounds = 0;
      pingNextTryAt = now + (PING_INTERVAL * 1000UL);
      return;
    }

    if (brokerReachable()) {
      // Sin ping pero el broker responde: se considera OK.
      pingFailRounds = 0;
      pingNextTryAt = now + (PING_INTERVAL * 1000UL);
      return;
    }

    // Sin ping Y sin broker: acumular fallos. Solo tras PING_FAIL_ROUNDS
    // rondas seguidas se reinicia con MAC nueva (watchdog de ping).
    pingFailRounds++;
    if (pingFailRounds >= PING_FAIL_ROUNDS) {
      rebootWithNewMac();
    }
    pingNextTryAt = now + (PING_INTERVAL * 1000UL);
    return;
  }

  if ((int32_t)(now - pingNextTryAt) >= 0) startPingRound();
}

bool brokerReachable() {
  WiFiClient tc;
  tc.setTimeout(2000);
  bool ok = tc.connect(MQTT_BROKER, MQTT_PORT);
  if (ok) tc.stop();
  ESP.wdtFeed();
  return ok;
}

#endif  // MODE_SERIAL_CSV == 0

// ═══════════════════════════════════════════════════════════════════════════
//  setup() / loop()  —  v4: ARRANQUE 100% SILENCIOSO
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
  ESP.wdtEnable(WDTO_8S);

  Serial.begin(BAUD_RATE);
  // v4: SIN banner, SIN [FW], SIN [RTC]

  memset(preRing, 0, sizeof(preRing));   // solo el anillo

#if MODE_SERIAL_CSV == 0
  loadEventCounter();                    // ANTES de imprimir (bug 1); silencioso

  rtcData_t rtc;
  ESP.rtcUserMemoryRead(0, (uint32_t*)&rtc, sizeof(rtc));   // I-001: 8 bytes
  if (rtc.magic == 0xDEAD && rtc.failureFlag == 1) {
    randomizeMac();
    rtc.failureFlag = 0;
    ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtc, sizeof(rtc));  // I-001: 8 bytes
  }
  wifiConnectRetries = 0;
  mqttPublishFailCount = 0;
  pingRoundActive = false;
  pingNextTryAt = 0;
  pingFailRounds = 0;
#endif

  // Inicializa el bus I2C directamente (v4: sin i2cBusRecover, ver nota).
  Wire.begin(PIN_SDA, PIN_SCL);

  if (accel.begin()) {
    accel.setRange(SENSOR_RANGE);
    accel.setDataRate(SENSOR_DATARATE);
    sensorOnline = true;
  } else {
    sensorOnline = false;
    notDetectedReported = true;
  }

  runChipDiagnostic();   // v4: SILENCIOSO
  ESP.wdtFeed();

#if MODE_SERIAL_CSV == 0
#if !MQTT_DRY_RUN
  // WiFi no bloqueante: solo iniciar; serviceWifi()/serviceMqtt() avanzan
  // la conexion desde loop() sin detener el muestreo.
  startWifi();
#endif
  // v4: SIN impresion de umbrales, ventana, broker ni setpoint
#endif
}

void loop() {
  ESP.wdtFeed();
  unsigned long now = millis();

  // =========================================================================
  // 1) MUESTREO PRIMERO (~100 Hz). Tiene prioridad sobre la red: las
  //    reconexiones WiFi/MQTT son no bloqueantes y no se pierden golpes.
  // =========================================================================
  // Muestreo interno a ~100 Hz
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    uint16_t dt = (uint16_t)(now - lastSampleMs);
    lastSampleMs = now;
    if (readAccel()) {
      pushSample(dt);       // 1) anillo del pasado (siempre, evento o no)
      processSample();      // 2) deteccion -> puede abrir una ventana
      advanceCapture(dt);   // 3) si hay ventana, actualiza el pico y cuenta POST
    }
  }

#if MODE_SERIAL_CSV == 1
  // Impresion serial a ~20 Hz: UNA linea CSV por tick
  if (setpointReady && (now - lastPrintMs >= PRINT_INTERVAL_MS)) {
    lastPrintMs = now;
    printCharTick();
  }
#endif

  // Reintento en background si el sensor no responde (silencioso). Re-hace
  // Wire.begin() a la misma velocidad que ya usa el sketch (v4: sin recovery).
  if (!sensorOnline && (now - lastRetryMs >= SENSOR_RETRY_MS)) {
    lastRetryMs = now;
    Wire.begin(PIN_SDA, PIN_SCL);
    if (accel.begin()) {
      accel.setRange(SENSOR_RANGE);
      accel.setDataRate(SENSOR_DATARATE);
      sensorOnline = true;
      notDetectedReported = false;
    }
  }

  // =========================================================================
  // 2) RED DESPUES (WiFi/MQTT/ping/heartbeat/stale) — no bloqueante
  // =========================================================================
#if MODE_SERIAL_CSV == 0
#if !MQTT_DRY_RUN
  serviceWifi();
  serviceMqtt();
  client.loop();
  servicePing();

  // Heartbeat: mantiene el "En linea" de la plataforma cada ~30 s.
  if (client.connected() && (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)) {
    lastHeartbeatMs = now;
    sendToBroker("report/online", "true");
  }

  // I-003: stale publish. Si no se logra publicar nada durante STALE_REPORT_MS
  // (15 min) y la red/MQTT no se recuperan, se reinicia con MAC aleatoria.
  if (now - lastStaleCheckMs >= 1000UL) {
    lastStaleCheckMs = now;
    if ((now - lastGoodPublish) > STALE_REPORT_MS) {
      rebootWithNewMac();   // v4: sin impresion
    }
  }
#endif
#endif
}
