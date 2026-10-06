#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <RadioLib.h>
#include <XPowersLib.h>
#include "lora_protocol.h"
#include "secrets.h"

static const int I2C_SDA_PIN = 21;
static const int I2C_SCL_PIN = 22;
static const uint32_t HEARTBEAT_INTERVAL_MS = 30000;

// Timeouts cortos: mientras dura el POST la base no procesa paquetes LoRa nuevos.
static const int32_t HTTP_CONNECT_TIMEOUT_MS = 2000;
static const uint16_t HTTP_RESPONSE_TIMEOUT_MS = 3000;
static const size_t RESPONSE_LOG_MAX_CHARS = 200;

XPowersLibInterface *pmu = nullptr;
SX1276 radio = new Module(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, LORA_DIO1_PIN);

volatile bool radioIrq = false;
bool radioReady = false;
bool wifiWasConnected = false;
uint32_t lastHeartbeatMs = 0;

uint32_t packetsAccepted = 0;
uint32_t packetsRejected = 0;
uint32_t packetsDuplicated = 0;
uint32_t postsOk = 0;
uint32_t postsFailed = 0;

// Ultimo paquete aceptado: si llega otro igual es un reintento del collar porque se perdio el ACK.
bool hasLastPacket = false;
char lastCollarId[COLLAR_ID_LEN] = {};
uint16_t lastSeq = 0;
uint32_t lastTimestamp = 0;

enum PacketResult { PACKET_REJECTED, PACKET_DUPLICATE, PACKET_NEW };

void IRAM_ATTR onRadioIrq() {
    radioIrq = true;
}

// La estacion base solo necesita el SX1276; el GPS se deja apagado para ahorrar energia.
bool powerOnRadio() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    pmu = new XPowersAXP2101(Wire);
    if (pmu->init()) {
        Serial.println("PMU AXP2101 detectado (T-Beam v1.2)");
        pmu->setPowerChannelVoltage(XPOWERS_ALDO2, 3300);  // VDD del SX1276
        pmu->enablePowerOutput(XPOWERS_ALDO2);
        pmu->disablePowerOutput(XPOWERS_ALDO3);             // GPS sin uso en la base
        return true;
    }
    delete pmu;

    pmu = new XPowersAXP192(Wire);
    if (pmu->init()) {
        Serial.println("PMU AXP192 detectado (T-Beam v1.0/v1.1)");
        pmu->setPowerChannelVoltage(XPOWERS_LDO2, 3300);   // VDD del SX1276
        pmu->enablePowerOutput(XPOWERS_LDO2);
        pmu->disablePowerOutput(XPOWERS_LDO3);              // GPS sin uso en la base
        return true;
    }
    delete pmu;
    pmu = nullptr;
    return false;
}

// collarId viaja como 16 bytes rellenados con ceros; se copia a un string terminado en '\0'.
void collarIdToString(const DataPacket &data, char out[COLLAR_ID_LEN + 1]) {
    memcpy(out, data.collarId, COLLAR_ID_LEN);
    out[COLLAR_ID_LEN] = '\0';
}

// El collarId llega por radio y termina dentro de un JSON: solo se aceptan [A-Za-z0-9_-].
bool isValidCollarId(const char *collarId) {
    if (collarId[0] == '\0') return false;
    for (const char *c = collarId; *c != '\0'; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') return false;
    }
    return true;
}

void formatTimestamp(uint32_t epoch, const char *format, char *out, size_t outSize) {
    time_t timestamp = (time_t)epoch;
    struct tm utc;
    gmtime_r(&timestamp, &utc);
    strftime(out, outSize, format, &utc);
}

bool sendAck(const DataPacket &data) {
    AckPacket ack = {};
    ack.version = PROTOCOL_VERSION;
    ack.type = MSG_TYPE_ACK;
    ack.seq = data.seq;
    memcpy(ack.collarId, data.collarId, COLLAR_ID_LEN);

    int16_t state = radio.transmit((uint8_t *)&ack, sizeof(ack));
    // DIO0 tambien se activa con TxDone: no debe confundirse con un paquete recibido.
    radioIrq = false;
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("ERROR: no se pudo enviar el ACK (codigo %d)\n", state);
        return false;
    }
    return true;
}

PacketResult receivePacket(DataPacket &data) {
    uint8_t buffer[256];
    size_t length = radio.getPacketLength();
    int16_t state = radio.readData(buffer, length);
    // Se leen antes de transmitir el ACK, que cambia el estado de la radio.
    float rssi = radio.getRSSI();
    float snr = radio.getSNR();
    float frequencyError = radio.getFrequencyError();

    if (state != RADIOLIB_ERR_NONE) {
        packetsRejected++;
        Serial.printf("Paquete descartado: error de recepcion (codigo %d; -7 = CRC invalido)\n", state);
        return PACKET_REJECTED;
    }
    if (length != sizeof(DataPacket)) {
        packetsRejected++;
        Serial.printf("Paquete descartado: tamano %u bytes (se esperaban %u)\n",
                      (unsigned)length, (unsigned)sizeof(DataPacket));
        return PACKET_REJECTED;
    }

    memcpy(&data, buffer, sizeof(data));
    if (data.version != PROTOCOL_VERSION || data.type != MSG_TYPE_DATA) {
        packetsRejected++;
        Serial.printf("Paquete descartado: version %u / tipo 0x%02X no soportados\n", data.version, data.type);
        return PACKET_REJECTED;
    }

    char collarId[COLLAR_ID_LEN + 1];
    collarIdToString(data, collarId);
    if (!isValidCollarId(collarId)) {
        packetsRejected++;
        Serial.println("Paquete descartado: collar_id con caracteres no permitidos");
        return PACKET_REJECTED;
    }

    // El ACK va primero: el collar solo lo espera ACK_TIMEOUT_MS.
    bool ackSent = sendAck(data);

    // Mismo seq y timestamp = reintento del mismo paquete. Comparar tambien el timestamp evita
    // tomar como duplicado un paquete nuevo si el collar se reinicio y su seq volvio a empezar.
    bool duplicate = hasLastPacket && data.seq == lastSeq && data.timestamp == lastTimestamp &&
                     memcmp(data.collarId, lastCollarId, COLLAR_ID_LEN) == 0;
    if (duplicate) {
        packetsDuplicated++;
    } else {
        packetsAccepted++;
        hasLastPacket = true;
        memcpy(lastCollarId, data.collarId, COLLAR_ID_LEN);
        lastSeq = data.seq;
        lastTimestamp = data.timestamp;
    }

    char timeText[24];
    formatTimestamp(data.timestamp, "%Y-%m-%d %H:%M:%S", timeText, sizeof(timeText));

    Serial.println("========================================");
    Serial.printf("Paquete recibido%s (%u bytes)\n", duplicate ? " [DUPLICADO - reintento del collar]" : "",
                  (unsigned)length);
    Serial.printf("  collar_id : %s\n", collarId);
    Serial.printf("  seq       : %u\n", data.seq);
    Serial.printf("  lat / lon : %.6f, %.6f\n", data.latE6 / 1e6, data.lonE6 / 1e6);
    Serial.printf("  timestamp : %lu (%s UTC)\n", (unsigned long)data.timestamp, timeText);
    Serial.printf("  satelites : %u\n", data.satellites);
    Serial.printf("  RSSI      : %.1f dBm | SNR: %.1f dB | error de frecuencia: %.0f Hz\n",
                  rssi, snr, frequencyError);
    Serial.printf("  ACK       : %s\n", ackSent ? "enviado" : "FALLO");
    Serial.printf("  totales   : aceptados %lu | duplicados %lu | descartados %lu\n",
                  (unsigned long)packetsAccepted, (unsigned long)packetsDuplicated,
                  (unsigned long)packetsRejected);

    return duplicate ? PACKET_DUPLICATE : PACKET_NEW;
}

// Un solo intento: si falla, el punto se pierde (sin reintentos, cola ni almacenamiento).
void forwardToBackend(const DataPacket &data) {
    if (WiFi.status() != WL_CONNECTED) {
        postsFailed++;
        Serial.println("  POST      : omitido, WiFi desconectado (se descarta)");
        return;
    }

    char collarId[COLLAR_ID_LEN + 1];
    collarIdToString(data, collarId);
    char isoTimestamp[24];
    formatTimestamp(data.timestamp, "%Y-%m-%dT%H:%M:%SZ", isoTimestamp, sizeof(isoTimestamp));

    // Mismos nombres y tipos que valida POST /locations: lat/lon como numero, timestamp ISO 8601.
    char body[160];
    snprintf(body, sizeof(body),
             "{\"collar_device_id\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"timestamp\":\"%s\"}",
             collarId, data.latE6 / 1e6, data.lonE6 / 1e6, isoTimestamp);

    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
    if (!http.begin(BACKEND_URL)) {
        postsFailed++;
        Serial.println("  POST      : ERROR, BACKEND_URL invalida en secrets.h (se descarta)");
        return;
    }
    http.addHeader("Content-Type", "application/json");

    uint32_t startMs = millis();
    int code = http.POST((uint8_t *)body, strlen(body));
    uint32_t elapsedMs = millis() - startMs;

    if (code <= 0) {
        postsFailed++;
        Serial.printf("  POST      : ERROR %s en %lu ms (se descarta)\n",
                      HTTPClient::errorToString(code).c_str(), (unsigned long)elapsedMs);
        http.end();
        return;
    }

    String response = http.getString();
    http.end();
    if (response.length() > RESPONSE_LOG_MAX_CHARS) {
        response = response.substring(0, RESPONSE_LOG_MAX_CHARS) + "...";
    }

    if (code == 201) {
        postsOk++;
        Serial.printf("  POST      : 201 Created en %lu ms | %s\n", (unsigned long)elapsedMs, response.c_str());
    } else {
        postsFailed++;
        Serial.printf("  POST      : %d en %lu ms (se descarta) | %s\n", code, (unsigned long)elapsedMs,
                      response.c_str());
    }
}

void logWifiChanges() {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected == wifiWasConnected) return;
    wifiWasConnected = connected;

    if (connected) {
        Serial.printf("[wifi] conectado a \"%s\" | IP %s | backend %s\n", WIFI_SSID,
                      WiFi.localIP().toString().c_str(), BACKEND_URL);
    } else {
        Serial.println("[wifi] desconectado (reconexion automatica activa)");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("Estacion base IoT - Parte 3: recepcion LoRa + reenvio HTTP al backend");

    if (!powerOnRadio()) {
        Serial.println("ATENCION: no se detecto PMU (AXP2101/AXP192); la radio podria no estar alimentada");
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[wifi] conectando a \"%s\"...\n", WIFI_SSID);

    int16_t state = beginLoraRadio(radio);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("ERROR: no se pudo iniciar el SX1276 (codigo %d)\n", state);
        return;
    }

    radio.setPacketReceivedAction(onRadioIrq);
    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("ERROR: no se pudo poner la radio en recepcion (codigo %d)\n", state);
        return;
    }

    radioReady = true;
    Serial.printf("LoRa SX1276 escuchando: %.1f MHz, SF%u, BW %.0f kHz\n", LORA_FREQUENCY_MHZ,
                  LORA_SPREADING_FACTOR, LORA_BANDWIDTH_KHZ);
}

void loop() {
    if (!radioReady) return;

    logWifiChanges();

    if (radioIrq) {
        radioIrq = false;
        DataPacket data;
        PacketResult result = receivePacket(data);

        // La radio vuelve a escuchar ANTES del POST: si llega otro paquete mientras tanto,
        // el SX1276 lo recibe solo y se procesa al terminar.
        radio.startReceive();

        if (result == PACKET_NEW) {
            forwardToBackend(data);
        } else if (result == PACKET_DUPLICATE) {
            Serial.println("  POST      : omitido (DUPLICADO)");
        }
    }

    if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = millis();
        Serial.printf("[base] escuchando... aceptados %lu | duplicados %lu | descartados %lu | "
                      "POST ok %lu | POST fallidos %lu | WiFi %s\n",
                      (unsigned long)packetsAccepted, (unsigned long)packetsDuplicated,
                      (unsigned long)packetsRejected, (unsigned long)postsOk, (unsigned long)postsFailed,
                      WiFi.status() == WL_CONNECTED ? "OK" : "sin conexion");
    }
}
