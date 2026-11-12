// secrets.example.h — Plantilla de credenciales (BusAccel-8266-v4)
// Copia este archivo como secrets.h y rellena con tus valores reales.
// secrets.h NO se versiona (esta en .gitignore).
#ifndef SECRETS_H
#define SECRETS_H

// STA: red WiFi
#define WIFI_SSID     "TU_RED_WIFI"
#define WIFI_PASSWORD "TU_CLAVE_WIFI"

// Broker MQTT.
//
// MQTT_CLIENT OBLIGATORIO DISTINTO por placa. El broker usa el ClientId como
// identidad: dos ESP8266 conectadas con el mismo ID se expulsan mutuamente de
// forma continua y ninguna de las dos publica de forma estable. Si tienes mas
// de una placa, dale a cada una un ID propio (por ejemplo el MAC de la placa).
#define MQTT_BROKER   "TU_BROKER"
#define MQTT_PORT     1883
#define MQTT_USERNAME "TU_USUARIO"
#define MQTT_PASSWORD "TU_CLAVE_MQTT"
#define MQTT_CLIENT   "TU_ID_UNICO_POR_PLACA"

#endif
