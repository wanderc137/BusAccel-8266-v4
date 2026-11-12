# BITÁCORA — BusAccel-8266-v4

Registro de problemas encontrados, decisiones técnicas y lecciones aprendidas
durante el desarrollo del firmware v4.

---

## 1. Problemas encontrados y cómo se resolvieron

### 1.1 Contrato MQTT: `report/peak` con JSON vs `report/value` texto plano

**Problema**: El PLAN.md especificaba originalmente `report/peak` con payload
JSON `{"peak":0.62}`. Sin embargo, el código real implementó `report/value`
con payload texto plano `"0.62"`.

**Solución**: Se documentó el contrato **real** del código (`report/value`
texto plano) en los tres archivos de documentación. Se añadió una nota en
datasheet.md indicando que `report/peak` fue sustituido por `report/value`
en v4.

**Lección**: El código es la fuente de verdad. Cuando hay discrepancia entre
el plan y la implementación, documentar lo que el código realmente hace.

### 1.2 Orden del loop(): muestreo vs red

**Problema**: La documentación original sugería que la red se servía antes
que el muestreo. El código real hace lo contrario: **muestreo primero, red
después**.

**Solución**: Se corrigió la documentación para reflejar el orden real:
1. Muestreo (~100 Hz): `pushSample() → processSample() → advanceCapture()`
2. Red (no bloqueante): `serviceWifi() → serviceMqtt() → client.loop() → servicePing()`

**Lección**: El orden es crítico. El muestreo tiene prioridad sobre la red
para no perder golpes durante reconexiones.

### 1.3 Arranque silencioso vs diagnóstico

**Problema**: El PLAN.md original especificaba un arranque con diagnóstico
impreso por serie. El código real implementa un arranque **100 % silencioso**.

**Solución**: Se documentó el comportamiento real: sin banner, sin
diagnóstico, sin setpoint impreso. La primera línea serie solo aparece tras
el primer evento detectado.

**Lección**: En producción, el ruido serie puede interferir con otros
sistemas. El silencio es una feature, no un bug.

### 1.4 Publicación condicional vs publicación de todos los eventos

**Problema**: El PLAN.md original sugería publicar todos los eventos por
MQTT. El código real implementa una **publicación condicional**: solo si el
pico supera estrictamente 0.58 g.

**Solución**: Se documentó el comportamiento real con una tabla clara:
- Pico ≤ 0.20 g: sin evento, sin impresión, sin publicación
- Pico > 0.20 g y ≤ 0.58 g: evento, **sí imprime**, **no publica**
- Pico > 0.58 g: evento, **sí imprime**, **sí publica**

**Lección**: La publicación condicional reduce el tráfico de red y evita
ruido en el broker. La impresión serie es independiente del umbral MQTT.

### 1.5 Contador RTC: validación de datos

**Problema**: La RTC user memory puede contener datos corruptos tras un
corte de luz o un reinicio inesperado.

**Solución**: Se implementó una validación reforzada:
- Magic correcto (`'EVT1'` = `0x45565431`)
- Valor dentro de rango (`≤ RTC_EVT_MAX = 100000000`)
- Valor no cero

**Lección**: La memoria persistente no es confiable. Siempre validar antes
de usar.

---

## 2. Vías o enfoques descartados

### 2.1 Publicación de todos los eventos por MQTT

**Enfoque descartado**: Publicar cada evento detectado por MQTT,
independientemente del pico.

**Por qué se descartó**: Generaría demasiado tráfico de red y ruido en el
broker. La publicación condicional (solo si pico > 0.58 g) es más eficiente.

### 2.2 Arranque con diagnóstico impreso

**Enfoque descartado**: Imprimir banner, estado del sensor, setpoint y
diagnóstico por serie al arrancar.

**Por qué se descartó**: En producción, el ruido serie puede interferir con
otros sistemas. El arranque silencioso es más robusto.

### 2.3 Detección por múltiples ejes

**Enfoque descartado**: Detectar eventos por desviación en X, Y o Z.

**Por qué se descartó**: El eje Z es el más relevante para golpes y
aceleraciones verticales en un bus. Los ejes X e Y se siguen leyendo pero
no disparan eventos.

### 2.4 Reconexión bloqueante

**Enfoque descartado**: Bloquear el loop mientras se reconecta WiFi/MQTT.

**Por qué se descartó**: Se perderían golpes durante la reconexión. La
reconexión no bloqueante es esencial para mantener el muestreo a ~100 Hz.

---

## 3. Decisiones técnicas relevantes

### 3.1 Publicación condicional (pico > 0.58 g)

**Decisión**: Solo publicar por MQTT si el pico supera estrictamente 0.58 g.

**Justificación**: Reduce el tráfico de red y evita ruido en el broker. Los
eventos menores se siguen detectando e imprimiendo por serie, pero no se
publican.

### 3.2 Arranque 100 % silencioso

**Decisión**: No imprimir nada por serie durante el arranque.

**Justificación**: En producción, el ruido serie puede interferir con otros
sistemas. El silencio es una feature, no un bug.

### 3.3 Muestreo prioritario sobre la red

**Decisión**: El muestreo (~100 Hz) tiene prioridad sobre la red en el loop.

**Justificación**: No se pierden golpes durante reconexiones WiFi/MQTT.

### 3.4 Contador de eventos en RTC user memory

**Decisión**: Almacenar el contador de eventos en la RTC user memory del
ESP8266.

**Justificación**: Sobrevive a reinicios por software (RTC alimentado). La
validación reforzada evita problemas con datos corruptos.

### 3.5 Watchdog de ping con reinicio por MAC aleatoria

**Decisión**: Si tras 3 rondas consecutivas no hay respuesta de ping y el
broker no es alcanzar, reiniciar con MAC aleatoria.

**Justificación**: En redes con DHCP, un conflicto de IP puede causar
problemas de conectividad. La MAC aleatoria fuerza una nueva asignación IP.

### 3.6 Contrato MQTT `report/` con texto plano

**Decisión**: Usar `report/value` con payload texto plano en lugar de
`report/peak` con JSON.

**Justificación**: El texto plano es más simple y ligero. La plataforma
SmartAge espera este formato.

---

## 4. Lecciones aprendidas

### 4.1 El código es la fuente de verdad

Cuando hay discrepancia entre el plan y la implementación, documentar lo que
el código realmente hace. No asumir que el plan es correcto.

### 4.2 El orden importa

El orden de las llamadas en el loop es crítico. El muestreo debe ir antes
que la red para no perder eventos.

### 4.3 La validación es esencial

La memoria persistente (RTC) no es confiable. Siempre validar antes de usar.

### 4.4 El silencio es una feature

En producción, el ruido serie puede interferir con otros sistemas. El
arranque silencioso es más robusto.

### 4.5 La publicación condicional reduce el tráfico

Publicar solo los eventos relevantes (pico > 0.58 g) reduce el tráfico de
red y evita ruido en el broker.

### 4.6 La reconexión no bloqueante es esencial

Bloquear el loop durante reconexiones WiFi/MQTT causaría pérdida de eventos.
La reconexión no bloqueante es esencial para mantener el muestreo.

---

## 5. Cambios v3 → v4

| Aspecto | v3 | v4 |
|---|---|---|
| Arranque | Con diagnóstico impreso | 100 % silencioso |
| Publicación MQTT | Todos los eventos | Solo si pico > 0.58 g |
| Salida serie | Múltiples líneas con símbolos | Una línea con el pico |
| Contrato MQTT | `report/peak` con JSON | `report/value` texto plano |
| Heartbeat | No | Cada 30 s |
| Watchdog de ping | No | Sí (3 rondas, reinicio MAC aleatoria) |
| Contador RTC | No | Sí (palabras 2-3, magic 'EVT1') |

---

## 6. Referencias

- `BusAccel-8266-v4.ino` — Sketch principal (915 líneas)
- `PLAN.md` — Plan de implementación
- `README.md` — Documentación general
- `docs/datasheet.md` — Documentación de usuario
- `docs/developer.md` — Documentación para desarrolladores
