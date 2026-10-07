#include <Arduino.h>
#include <Wire.h>
#include <cstddef>
#include <esp_sleep.h>
#include <RadioLib.h>
#include <TinyGPS++.h>
#include <XPowersLib.h>
#include "lora_protocol.h"

// Pinout segun LilyGO-LoRa-Series, examples/GPS/TinyGPS_Example/utilities.h (bloque T-Beam)
static const int GPS_RX_PIN = 34;  // RX del ESP32 <- TX del NEO-6M
static const int GPS_TX_PIN = 12;  // TX del ESP32 -> RX del NEO-6M
static const uint32_t GPS_BAUD = 9600;
static const int I2C_SDA_PIN = 21;
static const int I2C_SCL_PIN = 22;

// Identificador fijo de prueba; debe coincidir con animals.collar_device_id en el backend.
static const char COLLAR_ID[] = "COLLAR-TEST-1";
static_assert(sizeof(COLLAR_ID) <= COLLAR_ID_LEN, "COLLAR_ID no cabe en el paquete");

// ============================================================================
// RF8 - Intervalo de transmision configurable (se fija al programar el collar).
// Valor de produccion segun el documento de tesis: 60000 (60 s).
// Para depurar en escritorio se puede bajar temporalmente a 10000 (10 s).
// ============================================================================
static const uint32_t TX_INTERVAL_MS = 60000;

// ============================================================================
// SOLO DEPURACION. Con true, si no hay fix se guarda una lectura fija de prueba
// para probar el enlace LoRa en interiores. Usa el centro de la geocerca de
// prueba actual para no generar salidas (ni alertas de Telegram) falsas.
// PRODUCCION: false -> sin fix no se genera lectura.
// ============================================================================
static const bool DEBUG_SEND_WITHOUT_FIX = false;
static const double DEBUG_TEST_LAT = 7.762124;
static const double DEBUG_TEST_LON = -72.217737;
static const uint32_t DEBUG_TEST_EPOCH_BASE = 1790640000;  // 2026-09-29 00:00:00 UTC

// El GPS sigue encendido mientras el ESP32 duerme, asi que al despertar el fix llega en ~1 s.
static const uint32_t FIX_WAIT_MS = 5000;
// TinyGPS++ mantiene isValid() en true aunque se pierda el fix; la edad del dato lo delata.
static const uint32_t FIX_MAX_AGE_MS = 3000;

static const uint32_t ACK_TIMEOUT_MS = 2000;  // espera por cada intento
static const uint8_t MAX_RETRIES = 2;          // 1 intento + 2 reintentos

// RF3/RF5/RF6: buffer de lecturas no confirmadas por ACK.
static const uint16_t BUFFER_CAPACITY = 240;   // 4 h a un registro por minuto
static const uint8_t MAX_SENDS_PER_WAKE = 10;  // acota el tiempo despierto

struct BufferedReading {
    int32_t latE6;
    int32_t lonE6;
    uint32_t timestamp;
    uint16_t seq;  // se asigna al guardar y no cambia en los reintentos
    uint8_t satellites;
    uint8_t batteryPercent;
};
static_assert(sizeof(BufferedReading) == 16, "BufferedReading debe medir 16 bytes");

// Vive en memoria RTC: sobrevive al deep sleep (RF7) y a reinicios por software;
// solo se pierde si la placa se queda sin energia. magic + checksum distinguen un
// estado valido de la basura que queda en esa memoria al encender.
struct RtcState {
    uint32_t magic;
    uint32_t checksum;  // de todo lo que sigue a este campo
    uint32_t wakeCount;
    uint32_t droppedTotal;
    uint16_t nextSeq;
    uint16_t head;      // indice del registro mas antiguo
    uint16_t count;
    BufferedReading records[BUFFER_CAPACITY];
};

static const uint32_t RTC_STATE_MAGIC = 0xC0114A02;
RTC_NOINIT_ATTR RtcState rtcState;

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
XPowersLibInterface *pmu = nullptr;
SX1276 radio = new Module(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, LORA_DIO1_PIN);

volatile bool radioIrq = false;
bool radioReady = false;

String currentLine;

// Diagnostico de antena (trama GSV): disponible antes del fix, a diferencia de los satelites en uso.
int satsInView = -1;
int satsWithSignal = 0;
int cycleSatsWithSignal = 0;

void IRAM_ATTR onRadioIrq() {
    radioIrq = true;
}

uint32_t computeRtcChecksum() {
    // FNV-1a de 32 bits sobre todo lo que sigue al campo checksum.
    const uint8_t *data = reinterpret_cast<const uint8_t *>(&rtcState) + offsetof(RtcState, wakeCount);
    const size_t length = sizeof(RtcState) - offsetof(RtcState, wakeCount);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

// Devuelve false si no habia un estado valido (encendido en frio) y se inicio vacio.
bool restoreRtcState() {
    bool valid = rtcState.magic == RTC_STATE_MAGIC && rtcState.checksum == computeRtcChecksum() &&
                 rtcState.head < BUFFER_CAPACITY && rtcState.count <= BUFFER_CAPACITY;
    if (!valid) {
        memset(&rtcState, 0, sizeof(rtcState));
        rtcState.magic = RTC_STATE_MAGIC;
        rtcState.nextSeq = 1;
    }
    return valid;
}

void saveRtcState() {
    rtcState.checksum = computeRtcChecksum();
}

void bufferPush(const BufferedReading &reading) {
    if (rtcState.count == BUFFER_CAPACITY) {
        // Buffer lleno: se descarta el registro mas antiguo; la posicion reciente es la mas util.
        Serial.printf("Buffer lleno: se descarta el registro mas antiguo (seq=%u)\n",
                      rtcState.records[rtcState.head].seq);
        rtcState.head = (rtcState.head + 1) % BUFFER_CAPACITY;
        rtcState.count--;
        rtcState.droppedTotal++;
    }
    uint16_t tail = (rtcState.head + rtcState.count) % BUFFER_CAPACITY;
    rtcState.records[tail] = reading;
    rtcState.count++;
}

void bufferPopOldest() {
    rtcState.head = (rtcState.head + 1) % BUFFER_CAPACITY;
    rtcState.count--;
}

// En el T-Beam ni el GPS ni el SX1276 tienen alimentacion directa: la entrega el PMU
// (LilyGO-LoRa-Series, LoRaBoards.cpp). Al despertar las salidas siguen encendidas:
// volver a habilitarlas no corta la alimentacion del GPS.
bool powerOnPeripherals() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    pmu = new XPowersAXP2101(Wire);
    if (pmu->init()) {
        pmu->setPowerChannelVoltage(XPOWERS_ALDO2, 3300);    // VDD del SX1276
        pmu->enablePowerOutput(XPOWERS_ALDO2);
        pmu->setPowerChannelVoltage(XPOWERS_ALDO3, 3300);    // VDD del GPS
        pmu->enablePowerOutput(XPOWERS_ALDO3);
        pmu->setPowerChannelVoltage(XPOWERS_VBACKUP, 3300);  // respaldo del RTC del GPS
        pmu->enablePowerOutput(XPOWERS_VBACKUP);
    } else {
        delete pmu;
        pmu = new XPowersAXP192(Wire);
        if (!pmu->init()) {
            delete pmu;
            pmu = nullptr;
            return false;
        }
        pmu->setPowerChannelVoltage(XPOWERS_LDO2, 3300);     // VDD del SX1276
        pmu->enablePowerOutput(XPOWERS_LDO2);
        pmu->setPowerChannelVoltage(XPOWERS_LDO3, 3300);     // VDD del GPS
        pmu->enablePowerOutput(XPOWERS_LDO3);
    }

    // RF9: deteccion y medicion de bateria (igual que LoRaBoards.cpp de LilyGO).
    pmu->enableBattDetection();
    pmu->enableBattVoltageMeasure();
    return true;
}

// RF9: porcentaje del medidor de carga del PMU; BATTERY_UNKNOWN si no hay bateria.
uint8_t readBatteryPercent() {
    if (!pmu || !pmu->isBatteryConnect()) return BATTERY_UNKNOWN;
    int percent = pmu->getBatteryPercent();
    if (percent < 0 || percent > 100) return BATTERY_UNKNOWN;
    return (uint8_t)percent;
}

// $GPGSV,totalMsgs,msgNum,satsInView,[prn,elev,azim,snr] x hasta 4 satelites*checksum
void parseGsv(const String &sentence) {
    int checksumStart = sentence.indexOf('*');
    String body = checksumStart >= 0 ? sentence.substring(0, checksumStart) : sentence;

    int fieldIndex = 0;
    int start = 0;
    int totalMsgs = 0;
    int msgNum = 0;

    while (start <= (int)body.length()) {
        int comma = body.indexOf(',', start);
        if (comma < 0) comma = body.length();
        String field = body.substring(start, comma);

        if (fieldIndex == 1) {
            totalMsgs = field.toInt();
        } else if (fieldIndex == 2) {
            msgNum = field.toInt();
            if (msgNum == 1) cycleSatsWithSignal = 0;
        } else if (fieldIndex == 3) {
            satsInView = field.toInt();
        } else if (fieldIndex >= 7 && (fieldIndex - 7) % 4 == 0 && field.toInt() > 0) {
            cycleSatsWithSignal++;
        }

        fieldIndex++;
        start = comma + 1;
    }

    if (msgNum > 0 && msgNum == totalMsgs) satsWithSignal = cycleSatsWithSignal;
}

void readGps() {
    while (GPSSerial.available() > 0) {
        char c = GPSSerial.read();
        gps.encode(c);

        if (c == '\n') {
            if (currentLine.startsWith("$GPGSV")) parseGsv(currentLine);
            currentLine = "";
        } else if (c != '\r' && currentLine.length() < 120) {
            currentLine += c;
        }
    }
}

bool hasValidFix() {
    return gps.location.isValid() && gps.location.age() < FIX_MAX_AGE_MS;
}

// Con el campo NMEA vacio TinyGPS++ marca la hora como valida en 00:00:00; una fecha 0 nunca es real.
bool isTimeKnown() {
    return gps.time.isValid() && gps.date.isValid() && gps.date.value() != 0;
}

// days_from_civil (H. Hinnant): calcula el epoch en UTC sin depender de la zona horaria de mktime().
uint32_t toEpochUtc(int year, int month, int day, int hour, int minute, int second) {
    year -= month <= 2;
    const int era = year / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
    return days * 86400UL + hour * 3600UL + minute * 60UL + second;
}

// Lee el GPS hasta FIX_WAIT_MS; si hay fix, guarda la lectura al final del buffer (RF3).
void captureReading(uint8_t batteryPercent) {
    uint32_t start = millis();
    while (millis() - start < FIX_WAIT_MS && !(hasValidFix() && isTimeKnown())) {
        readGps();
    }

    BufferedReading reading = {};
    reading.batteryPercent = batteryPercent;

    if (hasValidFix() && isTimeKnown()) {
        reading.latE6 = (int32_t)lround(gps.location.lat() * 1e6);
        reading.lonE6 = (int32_t)lround(gps.location.lng() * 1e6);
        reading.timestamp = toEpochUtc(gps.date.year(), gps.date.month(), gps.date.day(),
                                       gps.time.hour(), gps.time.minute(), gps.time.second());
        uint32_t satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
        reading.satellites = (uint8_t)min<uint32_t>(satellites, 255);
        Serial.printf("GPS: fix en %lu ms | lat=%.6f lon=%.6f | sats=%u\n", (unsigned long)(millis() - start),
                      reading.latE6 / 1e6, reading.lonE6 / 1e6, reading.satellites);
    } else if (DEBUG_SEND_WITHOUT_FIX) {
        reading.latE6 = (int32_t)lround(DEBUG_TEST_LAT * 1e6);
        reading.lonE6 = (int32_t)lround(DEBUG_TEST_LON * 1e6);
        // millis() vuelve a 0 en cada despertar: el contador de despertares da timestamps crecientes.
        reading.timestamp = DEBUG_TEST_EPOCH_BASE + rtcState.wakeCount * (TX_INTERVAL_MS / 1000);
        Serial.println("GPS: sin fix -> lectura de PRUEBA (DEBUG_SEND_WITHOUT_FIX)");
    } else {
        Serial.printf("GPS: sin fix en %lu ms (satelites a la vista: %d, con senal: %d), no se genera lectura\n",
                      (unsigned long)FIX_WAIT_MS, satsInView, satsWithSignal);
        return;
    }

    reading.seq = rtcState.nextSeq++;
    bufferPush(reading);
}

bool isOurAck(const uint8_t *buffer, size_t length, const DataPacket &packet) {
    if (length != sizeof(AckPacket)) return false;
    AckPacket ack;
    memcpy(&ack, buffer, sizeof(ack));
    return ack.version == PROTOCOL_VERSION && ack.type == MSG_TYPE_ACK && ack.seq == packet.seq &&
           memcmp(ack.collarId, packet.collarId, COLLAR_ID_LEN) == 0;
}

bool sendWithAck(const DataPacket &packet) {
    const int totalAttempts = 1 + MAX_RETRIES;

    for (int attempt = 1; attempt <= totalAttempts; attempt++) {
        int16_t state = radio.transmit((uint8_t *)&packet, sizeof(packet));
        // DIO0 tambien se activa con TxDone: se descarta antes de escuchar el ACK.
        radioIrq = false;
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("  intento %d/%d: error de TX (codigo %d)\n", attempt, totalAttempts, state);
            continue;
        }

        radio.startReceive();
        uint32_t waitStart = millis();
        while (millis() - waitStart < ACK_TIMEOUT_MS) {
            if (!radioIrq) continue;
            radioIrq = false;

            uint8_t buffer[256];
            size_t length = radio.getPacketLength();
            int16_t rxState = radio.readData(buffer, length);
            if (rxState == RADIOLIB_ERR_NONE && isOurAck(buffer, length, packet)) {
                Serial.printf("  intento %d/%d: ACK recibido en %lu ms (RSSI %.1f dBm, SNR %.1f dB)\n",
                              attempt, totalAttempts, (unsigned long)(millis() - waitStart),
                              radio.getRSSI(), radio.getSNR());
                radio.standby();
                return true;
            }
            // No era nuestro ACK o llego corrupto: seguir escuchando hasta el timeout.
            radio.startReceive();
        }
        Serial.printf("  intento %d/%d: sin ACK en %lu ms\n", attempt, totalAttempts,
                      (unsigned long)ACK_TIMEOUT_MS);
    }

    radio.standby();
    return false;
}

// RF5/RF6: envia desde el registro mas antiguo; con ACK lo elimina, sin ACK se detiene
// para no alterar el orden cronologico (se reintenta en el proximo despertar).
void drainBuffer() {
    if (rtcState.count == 0) return;
    if (!radioReady) {
        Serial.printf("TX: radio LoRa no disponible; %u registro(s) quedan en el buffer\n", rtcState.count);
        return;
    }

    uint8_t sent = 0;
    while (rtcState.count > 0 && sent < MAX_SENDS_PER_WAKE) {
        const BufferedReading &reading = rtcState.records[rtcState.head];

        DataPacket packet = {};
        packet.version = PROTOCOL_VERSION;
        packet.type = MSG_TYPE_DATA;
        packet.seq = reading.seq;
        strncpy(packet.collarId, COLLAR_ID, COLLAR_ID_LEN);
        packet.latE6 = reading.latE6;
        packet.lonE6 = reading.lonE6;
        packet.timestamp = reading.timestamp;
        packet.satellites = reading.satellites;
        packet.batteryPercent = reading.batteryPercent;

        Serial.printf("TX seq=%u | lat=%.6f lon=%.6f | ts=%lu | sats=%u | bat=%u | pendientes=%u\n",
                      packet.seq, packet.latE6 / 1e6, packet.lonE6 / 1e6, (unsigned long)packet.timestamp,
                      packet.satellites, packet.batteryPercent, rtcState.count);

        if (!sendWithAck(packet)) {
            Serial.printf("TX seq=%u: sin ACK, queda en el buffer (pendientes=%u)\n", packet.seq, rtcState.count);
            return;
        }
        bufferPopOldest();
        sent++;
    }

    if (rtcState.count > 0) {
        Serial.printf("TX: limite de %u envios por despertar; pendientes=%u\n", MAX_SENDS_PER_WAKE, rtcState.count);
    }
}

// RF7: deep sleep hasta el proximo intervalo. El GPS (via PMU) sigue encendido.
void goToSleep() {
    if (radioReady) radio.sleep();
    saveRtcState();

    uint32_t awakeMs = millis();
    uint32_t sleepMs = awakeMs < TX_INTERVAL_MS ? TX_INTERVAL_MS - awakeMs : 1000;
    Serial.printf("Durmiendo %lu ms (despierto %lu ms) | buffer=%u/%u\n", (unsigned long)sleepMs,
                  (unsigned long)awakeMs, rtcState.count, BUFFER_CAPACITY);
    Serial.flush();

    esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
    esp_deep_sleep_start();
}

// Con deep sleep cada despertar reinicia el programa: todo el ciclo ocurre en setup().
void setup() {
    Serial.begin(115200);

    bool restored = restoreRtcState();
    rtcState.wakeCount++;
    bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

    Serial.println();
    Serial.printf("=== %s | despertar #%lu (%s) | buffer=%u/%u%s | descartados por buffer lleno=%lu ===\n",
                  COLLAR_ID, (unsigned long)rtcState.wakeCount, timerWake ? "temporizador" : "encendido",
                  rtcState.count, BUFFER_CAPACITY, restored ? "" : " (estado nuevo)",
                  (unsigned long)rtcState.droppedTotal);

    if (!powerOnPeripherals()) {
        Serial.println("ATENCION: no se detecto PMU (AXP2101/AXP192); GPS y LoRa podrian no estar alimentados");
    }

    uint8_t batteryPercent = readBatteryPercent();
    if (batteryPercent == BATTERY_UNKNOWN) {
        Serial.println("Bateria: no conectada");
    } else {
        Serial.printf("Bateria: %u%% (%u mV)\n", batteryPercent, pmu->getBattVoltage());
    }

    GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    int16_t state = beginLoraRadio(radio);
    if (state == RADIOLIB_ERR_NONE) {
        radio.setPacketReceivedAction(onRadioIrq);
        radioReady = true;
    } else {
        Serial.printf("ERROR: no se pudo iniciar el SX1276 (codigo %d)\n", state);
    }

    captureReading(batteryPercent);
    drainBuffer();
    goToSleep();
}

void loop() {
    // No se alcanza: goToSleep() entra en deep sleep al final de setup().
}
