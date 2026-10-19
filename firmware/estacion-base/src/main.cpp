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

// Timeouts cortos: mientras dura un POST la base no procesa paquetes LoRa nuevos.
static const int32_t HTTP_CONNECT_TIMEOUT_MS = 2000;
static const uint16_t HTTP_RESPONSE_TIMEOUT_MS = 3000;
static const size_t RESPONSE_LOG_MAX_CHARS = 200;

// Cola de reenvio al backend: si el POST falla por red/backend, se reintenta en orden.
static const uint16_t QUEUE_CAPACITY = 500;
static const uint32_t POST_RETRY_INTERVAL_MS = 10000;

// Duplicados: ultimo registro aceptado de cada collar (reintentos por ACK perdido).
static const uint8_t MAX_TRACKED_COLLARS = 16;

XPowersLibInterface *pmu = nullptr;
SX1276 radio = new Module(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, LORA_DIO1_PIN);

volatile bool radioIrq = false;
bool radioReady = false;
bool wifiWasConnected = false;
uint32_t lastHeartbeatMs = 0;
uint32_t nextPostAttemptMs = 0;

uint32_t packetsAccepted = 0;
uint32_t packetsRejected = 0;
uint32_t packetsDuplicated = 0;
uint32_t postsOk = 0;
uint32_t postsRetried = 0;
uint32_t postsDiscarded = 0;
uint32_t queueDropped = 0;

struct CollarLastSeen {
    bool used;
    char collarId[COLLAR_ID_LEN];
    uint16_t seq;
    uint32_t timestamp;
};
CollarLastSeen lastSeen[MAX_TRACKED_COLLARS] = {};
uint8_t nextLastSeenSlot = 0;

struct QueuedReading {
    char collarId[COLLAR_ID_LEN + 1];
    uint16_t seq;
    int32_t latE6;
    int32_t lonE6;
    uint32_t timestamp;
    uint8_t satellites;
    uint8_t batteryPercent;
    float rssi;  // medidos por esta base al recibir (OE4: alcance del enlace)
    float snr;
};
QueuedReading queue[QUEUE_CAPACITY];
uint16_t queueHead = 0;
uint16_t queueCount = 0;

enum PacketResult { PACKET_REJECTED, PACKET_DUPLICATE, PACKET_NEW };
enum PostResult { POST_OK, POST_DISCARD, POST_RETRY };

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

// v1 (33 bytes, sin bateria) se convierte a la forma v2 con bateria desconocida.
bool parseDataPacket(const uint8_t *buffer, size_t length, DataPacket &data) {
    if (length == sizeof(DataPacket) && buffer[0] == PROTOCOL_VERSION) {
        memcpy(&data, buffer, sizeof(data));
        return true;
    }
    if (length == sizeof(DataPacketV1) && buffer[0] == PROTOCOL_VERSION_V1) {
        DataPacketV1 v1;
        memcpy(&v1, buffer, sizeof(v1));
        data.version = v1.version;
        data.type = v1.type;
        data.seq = v1.seq;
        memcpy(data.collarId, v1.collarId, COLLAR_ID_LEN);
        data.latE6 = v1.latE6;
        data.lonE6 = v1.lonE6;
        data.timestamp = v1.timestamp;
        data.satellites = v1.satellites;
        data.batteryPercent = BATTERY_UNKNOWN;
        return true;
    }
    return false;
}

bool sendAck(const DataPacket &data) {
    AckPacket ack = {};
    ack.version = data.version;  // misma version recibida: un collar v1 acepta el ACK
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

// Mismo seq y timestamp que el ultimo registro de ese collar = reintento por ACK perdido.
// Comparar el timestamp evita confundir un registro nuevo si el seq del collar se reinicio.
bool isDuplicateAndRemember(const DataPacket &data) {
    for (uint8_t i = 0; i < MAX_TRACKED_COLLARS; i++) {
        CollarLastSeen &entry = lastSeen[i];
        if (!entry.used || memcmp(entry.collarId, data.collarId, COLLAR_ID_LEN) != 0) continue;
        if (entry.seq == data.seq && entry.timestamp == data.timestamp) return true;
        entry.seq = data.seq;
        entry.timestamp = data.timestamp;
        return false;
    }

    CollarLastSeen *slot = nullptr;
    for (uint8_t i = 0; i < MAX_TRACKED_COLLARS && !slot; i++) {
        if (!lastSeen[i].used) slot = &lastSeen[i];
    }
    if (!slot) {
        slot = &lastSeen[nextLastSeenSlot];
        nextLastSeenSlot = (nextLastSeenSlot + 1) % MAX_TRACKED_COLLARS;
    }
    slot->used = true;
    memcpy(slot->collarId, data.collarId, COLLAR_ID_LEN);
    slot->seq = data.seq;
    slot->timestamp = data.timestamp;
    return false;
}

void enqueue(const DataPacket &data, float rssi, float snr) {
    if (queueCount == QUEUE_CAPACITY) {
        // Cola llena: se descarta el registro mas antiguo.
        Serial.printf("  cola      : llena, se descarta el mas antiguo (%s ts=%lu)\n", queue[queueHead].collarId,
                      (unsigned long)queue[queueHead].timestamp);
        queueHead = (queueHead + 1) % QUEUE_CAPACITY;
        queueCount--;
        queueDropped++;
    }

    QueuedReading &item = queue[(queueHead + queueCount) % QUEUE_CAPACITY];
    collarIdToString(data, item.collarId);
    item.seq = data.seq;
    item.latE6 = data.latE6;
    item.lonE6 = data.lonE6;
    item.timestamp = data.timestamp;
    item.satellites = data.satellites;
    item.batteryPercent = data.batteryPercent;
    item.rssi = rssi;
    item.snr = snr;
    queueCount++;
}

void dequeueOldest() {
    queueHead = (queueHead + 1) % QUEUE_CAPACITY;
    queueCount--;
}

PacketResult receivePacket() {
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

    DataPacket data;
    if (!parseDataPacket(buffer, length, data)) {
        packetsRejected++;
        Serial.printf("Paquete descartado: %u bytes, version %u (se aceptan v%u de %u bytes y v%u de %u bytes)\n",
                      (unsigned)length, length > 0 ? buffer[0] : 0, PROTOCOL_VERSION_V1,
                      (unsigned)sizeof(DataPacketV1), PROTOCOL_VERSION, (unsigned)sizeof(DataPacket));
        return PACKET_REJECTED;
    }
    if (data.type != MSG_TYPE_DATA) {
        packetsRejected++;
        Serial.printf("Paquete descartado: tipo 0x%02X no soportado\n", data.type);
        return PACKET_REJECTED;
    }

    char collarId[COLLAR_ID_LEN + 1];
    collarIdToString(data, collarId);
    if (!isValidCollarId(collarId)) {
        packetsRejected++;
        Serial.println("Paquete descartado: collar_id con caracteres no permitidos");
        return PACKET_REJECTED;
    }

    // El ACK va primero: el collar solo lo espera ACK_TIMEOUT_MS. El ACK significa
    // "la base lo recibio"; el reenvio al backend lo garantiza la cola de la base.
    bool ackSent = sendAck(data);

    bool duplicate = isDuplicateAndRemember(data);
    if (duplicate) {
        packetsDuplicated++;
    } else {
        packetsAccepted++;
        enqueue(data, rssi, snr);
    }

    char timeText[24];
    formatTimestamp(data.timestamp, "%Y-%m-%d %H:%M:%S", timeText, sizeof(timeText));

    Serial.println("========================================");
    Serial.printf("Paquete recibido v%u%s (%u bytes)\n", data.version,
                  duplicate ? " [DUPLICADO - reintento del collar]" : "", (unsigned)length);
    Serial.printf("  collar_id : %s\n", collarId);
    Serial.printf("  seq       : %u\n", data.seq);
    Serial.printf("  lat / lon : %.6f, %.6f\n", data.latE6 / 1e6, data.lonE6 / 1e6);
    Serial.printf("  timestamp : %lu (%s UTC)\n", (unsigned long)data.timestamp, timeText);
    Serial.printf("  satelites : %u\n", data.satellites);
    if (data.batteryPercent == BATTERY_UNKNOWN) {
        Serial.println("  bateria   : sin dato");
    } else {
        Serial.printf("  bateria   : %u%%\n", data.batteryPercent);
    }
    Serial.printf("  RSSI      : %.1f dBm | SNR: %.1f dB | error de frecuencia: %.0f Hz\n",
                  rssi, snr, frequencyError);
    Serial.printf("  ACK       : %s\n", ackSent ? "enviado" : "FALLO");
    Serial.printf("  cola      : %s (pendientes %u/%u)\n", duplicate ? "omitido (DUPLICADO)" : "encolado",
                  queueCount, QUEUE_CAPACITY);
    // RNF8: misma informacion en una sola linea facil de parsear.
    Serial.printf("[RX] collar=%s seq=%u rssi=%.1f snr=%.2f duplicado=%s\n", collarId, data.seq, rssi, snr,
                  duplicate ? "si" : "no");

    return duplicate ? PACKET_DUPLICATE : PACKET_NEW;
}

// Un intento por llamada. 201 = guardado; 200 = ya estaba guardado (p. ej. un intento anterior
// que dio timeout si llego al backend): tambien es exito. 400/404 = el backend lo rechazaria
// siempre (se descarta para no trabar la cola); cualquier otro fallo = se reintenta.
PostResult postReading(const QueuedReading &item) {
    char isoTimestamp[24];
    formatTimestamp(item.timestamp, "%Y-%m-%dT%H:%M:%SZ", isoTimestamp, sizeof(isoTimestamp));

    char battery[8];
    if (item.batteryPercent == BATTERY_UNKNOWN) {
        strcpy(battery, "null");
    } else {
        snprintf(battery, sizeof(battery), "%u", item.batteryPercent);
    }

    // Mismos nombres y tipos que valida POST /locations: lat/lon como numero, timestamp ISO 8601.
    char body[300];
    snprintf(body, sizeof(body),
             "{\"collar_device_id\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"timestamp\":\"%s\",\"battery_percent\":%s,"
             "\"satellites\":%u,\"packet_seq\":%u,\"rssi\":%.1f,\"snr\":%.2f}",
             item.collarId, item.latE6 / 1e6, item.lonE6 / 1e6, isoTimestamp, battery, item.satellites, item.seq,
             item.rssi, item.snr);

    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
    if (!http.begin(BACKEND_URL)) {
        Serial.println("POST: ERROR, BACKEND_URL invalida en secrets.h");
        return POST_RETRY;
    }
    http.addHeader("Content-Type", "application/json");

    uint32_t startMs = millis();
    int code = http.POST((uint8_t *)body, strlen(body));
    uint32_t elapsedMs = millis() - startMs;

    if (code <= 0) {
        Serial.printf("POST [%s seq=%u]: ERROR %s en %lu ms\n", item.collarId, item.seq,
                      HTTPClient::errorToString(code).c_str(), (unsigned long)elapsedMs);
        http.end();
        return POST_RETRY;
    }

    String response = http.getString();
    http.end();
    if (response.length() > RESPONSE_LOG_MAX_CHARS) {
        response = response.substring(0, RESPONSE_LOG_MAX_CHARS) + "...";
    }

    Serial.printf("POST [%s seq=%u]: %d en %lu ms | %s\n", item.collarId, item.seq, code,
                  (unsigned long)elapsedMs, response.c_str());
    if (code == 201 || code == 200) return POST_OK;
    if (code == 400 || code == 404) return POST_DISCARD;
    return POST_RETRY;
}

void processQueue() {
    if (queueCount == 0 || WiFi.status() != WL_CONNECTED) return;
    if ((int32_t)(millis() - nextPostAttemptMs) < 0) return;

    PostResult result = postReading(queue[queueHead]);
    if (result == POST_RETRY) {
        postsRetried++;
        nextPostAttemptMs = millis() + POST_RETRY_INTERVAL_MS;
        Serial.printf("  -> queda en la cola, se reintenta en %lu s (pendientes %u)\n",
                      (unsigned long)(POST_RETRY_INTERVAL_MS / 1000), queueCount);
        return;
    }

    if (result == POST_OK) {
        postsOk++;
    } else {
        postsDiscarded++;
        Serial.println("  -> el backend lo rechaza: se descarta de la cola");
    }
    dequeueOldest();
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
    Serial.println("Estacion base IoT - recepcion LoRa (v1/v2) + cola de reenvio HTTP al backend");

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
        receivePacket();
        // Vuelve a escuchar antes de cualquier POST: si llega otro paquete mientras tanto,
        // el SX1276 lo recibe solo y se procesa en la siguiente vuelta.
        radio.startReceive();
    }

    // Un POST como maximo por vuelta, para atender la radio entre envios.
    processQueue();

    if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = millis();
        Serial.printf("[base] aceptados %lu | duplicados %lu | descartados %lu | cola %u/%u | "
                      "POST ok %lu | reintentos %lu | rechazados %lu | WiFi %s\n",
                      (unsigned long)packetsAccepted, (unsigned long)packetsDuplicated,
                      (unsigned long)packetsRejected, queueCount, QUEUE_CAPACITY, (unsigned long)postsOk,
                      (unsigned long)postsRetried, (unsigned long)postsDiscarded,
                      WiFi.status() == WL_CONNECTED ? "OK" : "sin conexion");
    }
}
