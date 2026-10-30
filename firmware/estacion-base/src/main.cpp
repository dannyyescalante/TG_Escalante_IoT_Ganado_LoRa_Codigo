#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <esp_system.h>
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

// Cola de reenvio al backend (RF12): si el POST falla por red/backend, se reintenta en orden.
// Vive en RAM: sin WiFi guarda hasta 500 lecturas (~8 h con un collar a 1 lectura/min).
// Si se llena, se descarta la mas antigua (ver enqueue). Un reinicio la borra.
static const uint16_t QUEUE_CAPACITY = 500;
// Espera creciente entre reintentos de POST (tope 30 s): un backend apagado no se consulta en rafaga.
static const uint32_t POST_RETRY_MS[] = {10000, 20000, 30000};
static const uint8_t POST_RETRY_STEPS = sizeof(POST_RETRY_MS) / sizeof(POST_RETRY_MS[0]);

// Reconexion WiFi propia (RF12): espera creciente entre intentos, sin bloquear la radio.
static const uint32_t WIFI_BACKOFF_MS[] = {2000, 5000, 10000, 20000, 30000};
static const uint8_t WIFI_BACKOFF_STEPS = sizeof(WIFI_BACKOFF_MS) / sizeof(WIFI_BACKOFF_MS[0]);
// POST seguidos sin respuesta de red con el WiFi "conectado" = asociacion vieja: se fuerza reconexion.
static const uint8_t POST_NET_FAIL_LIMIT = 3;
// Ultimo recurso: reiniciar si el WiFi no vuelve en este tiempo Y la red esta visible (pila WiFi trabada).
static const uint32_t WIFI_RESTART_AFTER_MS = 5UL * 60UL * 1000UL;
static const uint32_t RESTART_TRACE_MAGIC = 0x57494649;  // "WIFI"

// Duplicados: ultimo registro aceptado de cada collar (reintentos por ACK perdido).
static const uint8_t MAX_TRACKED_COLLARS = 16;

XPowersLibInterface *pmu = nullptr;
SX1276 radio = new Module(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, LORA_DIO1_PIN);

volatile bool radioIrq = false;
bool radioReady = false;
uint32_t lastHeartbeatMs = 0;
uint32_t nextPostAttemptMs = 0;
uint16_t postFailStreak = 0;   // POST fallidos seguidos (cualquier motivo): define la espera del reintento
uint8_t postNetFailures = 0;   // POST seguidos sin respuesta de red (code <= 0)

// Estado del WiFi segun este firmware. WiFi.status() solo no basta: en campo siguio en
// WL_CONNECTED mientras el punto de acceso ya no tenia a la base asociada.
enum WifiState { WIFI_UP, WIFI_DOWN };
WifiState wifiState = WIFI_DOWN;
bool wifiEverConnected = false;
uint32_t wifiDownSinceMs = 0;
uint32_t nextWifiAttemptMs = 0;
uint16_t wifiAttempts = 0;
bool restartScanStarted = false;
volatile bool wifiGotIp = false;  // lo escribe el manejador de eventos WiFi (otra tarea)

// Motivo del ultimo reinicio por WiFi: RTC_NOINIT sobrevive a ESP.restart(), pero tras un
// encendido en frio trae basura, por eso se valida con magic al arrancar.
struct RestartTrace {
    uint32_t magic;
    uint32_t restarts;      // reinicios por WiFi desde el ultimo encendido en frio
    uint32_t lastLost;      // lecturas en cola que se perdieron en el ultimo reinicio
    uint32_t pending;       // 1 = el proximo arranque viene de un reinicio por WiFi
};
RTC_NOINIT_ATTR RestartTrace restartTrace;

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

static uint8_t stepIndex(uint16_t n, uint8_t steps) {
    return n < steps ? (uint8_t)n : (uint8_t)(steps - 1);
}

void onWifiEvent(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        wifiGotIp = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED || event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
        wifiGotIp = false;
    }
}

// Pasa a "sin WiFi" y programa el primer intento de reconexion. Reinicia el conteo de fallos de POST
// para que la proxima reconexion forzada necesite otros POST_NET_FAIL_LIMIT fallos (espaciados por su espera).
void markWifiDown(const char *reason) {
    postNetFailures = 0;
    if (wifiState == WIFI_DOWN) return;
    wifiState = WIFI_DOWN;
    wifiDownSinceMs = millis();
    wifiAttempts = 0;
    nextWifiAttemptMs = millis();
    restartScanStarted = false;
    Serial.printf("[WIFI] desconexion detectada (%s) | cola %u/%u\n", reason, queueCount, QUEUE_CAPACITY);
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
        // Sin respuesta de red: varias seguidas con el WiFi "conectado" = asociacion vieja.
        if (++postNetFailures >= POST_NET_FAIL_LIMIT) {
            markWifiDown("POST seguidos sin respuesta de red; se fuerza la reconexion");
        }
        return POST_RETRY;
    }
    postNetFailures = 0;  // hubo respuesta HTTP: la red funciona (aunque el backend responda error)

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
    if (queueCount == 0 || wifiState != WIFI_UP) return;
    if ((int32_t)(millis() - nextPostAttemptMs) < 0) return;

    PostResult result = postReading(queue[queueHead]);
    if (result == POST_RETRY) {
        postsRetried++;
        uint32_t waitMs = POST_RETRY_MS[stepIndex(postFailStreak, POST_RETRY_STEPS)];
        postFailStreak++;
        nextPostAttemptMs = millis() + waitMs;
        Serial.printf("  -> queda en la cola, se reintenta en %lu s (pendientes %u)\n",
                      (unsigned long)(waitMs / 1000), queueCount);
        return;
    }

    postFailStreak = 0;
    if (result == POST_OK) {
        postsOk++;
    } else {
        postsDiscarded++;
        Serial.println("  -> el backend lo rechaza: se descarta de la cola");
    }
    dequeueOldest();
}

// Ultimo recurso. Se imprime cada lectura que se va a perder, para que quede rastro en el log.
void restartForWifi() {
    Serial.printf("[WIFI] reinicio: la red esta visible pero no conecta hace %lu min | se pierden %u lecturas de la cola\n",
                  (unsigned long)((millis() - wifiDownSinceMs) / 60000), queueCount);
    for (uint16_t i = 0; i < queueCount; i++) {
        const QueuedReading &r = queue[(queueHead + i) % QUEUE_CAPACITY];
        Serial.printf("[WIFI] perdida por reinicio: collar=%s seq=%u ts=%lu\n", r.collarId, r.seq,
                      (unsigned long)r.timestamp);
    }
    restartTrace.restarts++;
    restartTrace.lastLost = queueCount;
    restartTrace.pending = 1;
    Serial.flush();
    ESP.restart();
}

// Tras WIFI_RESTART_AFTER_MS sin WiFi se busca la red (escaneo asincrono, no bloquea la radio).
// No visible = router/hotspot apagado: no se reinicia, para conservar la cola, y se vuelve a evaluar
// en otro periodo. Visible pero sin conexion = pila WiFi trabada: se reinicia.
// Devuelve true mientras el escaneo esta en curso (no se intenta conectar para no cortarlo).
bool considerRestart() {
    if (!restartScanStarted) {
        WiFi.disconnect();
        WiFi.scanNetworks(true);
        restartScanStarted = true;
        Serial.printf("[WIFI] %lu min sin WiFi: se busca la red para decidir si reiniciar | cola %u/%u\n",
                      (unsigned long)((millis() - wifiDownSinceMs) / 60000), queueCount, QUEUE_CAPACITY);
        return true;
    }

    int16_t found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return true;

    bool visible = false;
    for (int16_t i = 0; i < found; i++) {
        if (WiFi.SSID(i) == WIFI_SSID) {
            visible = true;
            break;
        }
    }
    WiFi.scanDelete();
    restartScanStarted = false;

    if (!visible) {
        Serial.printf("[WIFI] la red no esta visible tras %lu min sin WiFi: no se reinicia (se conserva la cola %u/%u)\n",
                      (unsigned long)((millis() - wifiDownSinceMs) / 60000), queueCount, QUEUE_CAPACITY);
        wifiDownSinceMs = millis();
        return false;
    }
    restartForWifi();
    return true;
}

// Se llama en cada vuelta de loop(); nada aqui espera a que el WiFi conecte.
void maintainWifi() {
    uint32_t now = millis();

    if (wifiState == WIFI_UP) {
        if (WiFi.status() != WL_CONNECTED || !wifiGotIp) {
            markWifiDown("el ESP32 reporta WiFi caido");
        }
        return;
    }

    // wifiGotIp lo pone en false cada intento: se exige una IP obtenida despues del ultimo intento,
    // no un WL_CONNECTED que quedo de antes del disconnect().
    if (wifiAttempts > 0 && wifiGotIp && WiFi.status() == WL_CONNECTED) {
        wifiState = WIFI_UP;
        Serial.printf("[WIFI] %s tras %u intento(s) en %lu s | IP %s | RSSI %d dBm | cola %u/%u\n",
                      wifiEverConnected ? "reconectado" : "conectado", wifiAttempts,
                      (unsigned long)((now - wifiDownSinceMs) / 1000), WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI(), queueCount, QUEUE_CAPACITY);
        wifiEverConnected = true;
        // Si el POST venia fallando (p. ej. backend apagado), se respeta su espera; si no, la cola se vacia ya.
        if (postFailStreak == 0) nextPostAttemptMs = now;
        return;
    }

    if (now - wifiDownSinceMs >= WIFI_RESTART_AFTER_MS || restartScanStarted) {
        if (considerRestart()) return;
    }

    if ((int32_t)(now - nextWifiAttemptMs) >= 0) {
        uint32_t waitMs = WIFI_BACKOFF_MS[stepIndex(wifiAttempts, WIFI_BACKOFF_STEPS)];
        wifiAttempts++;
        wifiGotIp = false;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        nextWifiAttemptMs = now + waitMs;
        Serial.printf("[WIFI] intento %u de conexion | siguiente en %lu s | sin WiFi hace %lu s | cola %u/%u\n",
                      wifiAttempts, (unsigned long)(waitMs / 1000), (unsigned long)((now - wifiDownSinceMs) / 1000),
                      queueCount, QUEUE_CAPACITY);
    }
}

// Valida la traza del ultimo reinicio: tras un encendido en frio RTC_NOINIT trae basura.
void initRestartTrace() {
    if (restartTrace.magic != RESTART_TRACE_MAGIC) {
        memset(&restartTrace, 0, sizeof(restartTrace));
        restartTrace.magic = RESTART_TRACE_MAGIC;
        Serial.println("[WIFI] arranque en frio: contador de reinicios por WiFi en 0");
        return;
    }
    if (restartTrace.pending && esp_reset_reason() == ESP_RST_SW) {
        Serial.printf("[WIFI] arranque tras reinicio por falta de WiFi (#%lu desde el encendido; %lu lecturas perdidas)\n",
                      (unsigned long)restartTrace.restarts, (unsigned long)restartTrace.lastLost);
    }
    restartTrace.pending = 0;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("Estacion base IoT - recepcion LoRa (v1/v2) + cola de reenvio HTTP al backend");

    if (!powerOnRadio()) {
        Serial.println("ATENCION: no se detecto PMU (AXP2101/AXP192); la radio podria no estar alimentada");
    }

    initRestartTrace();

    // La conexion y las reconexiones las maneja maintainWifi() desde loop(), sin bloquear la radio.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(onWifiEvent);
    wifiState = WIFI_DOWN;
    wifiDownSinceMs = millis();
    nextWifiAttemptMs = millis();
    Serial.println("[WIFI] conectando (red configurada en secrets.h)");

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

    maintainWifi();

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
                      "descartadas por cola llena %lu | POST ok %lu | reintentos %lu | rechazados %lu | WiFi %s\n",
                      (unsigned long)packetsAccepted, (unsigned long)packetsDuplicated,
                      (unsigned long)packetsRejected, queueCount, QUEUE_CAPACITY, (unsigned long)queueDropped,
                      (unsigned long)postsOk, (unsigned long)postsRetried, (unsigned long)postsDiscarded,
                      wifiState == WIFI_UP ? "OK" : "sin conexion");
    }
}
