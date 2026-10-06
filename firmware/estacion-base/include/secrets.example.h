#pragma once

// Plantilla de configuracion local de la estacion base.
// Copiar este archivo como "secrets.h" (en esta misma carpeta) y poner los valores reales.
// secrets.h esta en .gitignore: nunca se sube al repositorio.

// Red WiFi de 2.4 GHz (el ESP32 no soporta 5 GHz).
static const char WIFI_SSID[] = "NOMBRE_DE_TU_RED";
static const char WIFI_PASSWORD[] = "CLAVE_DE_TU_RED";

// URL del endpoint de ingesta del backend. Usar la IP de la PC en la red local
// (ipconfig -> Adaptador de LAN inalambrica Wi-Fi -> Direccion IPv4), no "localhost".
static const char BACKEND_URL[] = "http://192.168.1.50:3018/locations";
