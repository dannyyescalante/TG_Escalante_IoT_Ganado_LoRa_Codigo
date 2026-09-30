#include <Arduino.h>
#include <Wire.h>
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

static const uint32_t REPORT_INTERVAL_MS = 2000;
// TinyGPS++ mantiene isValid() en true aunque se pierda el fix; la edad del dato lo delata.
static const uint32_t FIX_MAX_AGE_MS = 3000;
static const uint32_t NO_DATA_WARNING_AFTER_MS = 5000;

// Identificador fijo de prueba; debe coincidir con animals.collar_device_id en el backend.
static const char COLLAR_ID[] = "COLLAR-TEST-1";
static_assert(sizeof(COLLAR_ID) <= COLLAR_ID_LEN, "COLLAR_ID no cabe en el paquete");

// ============================================================================
// RF8 - Intervalo de transmision configurable.
// 10 s SOLO para depuracion en escritorio. Valor de produccion segun el
// documento de tesis: 60000 (60 s). Cambiar aqui antes de las pruebas de campo.
// ============================================================================
static const uint32_t TX_INTERVAL_MS = 10000;

// ============================================================================
// SOLO DEPURACION. Con true, si no hay fix se transmite una posicion fija de
// prueba (centro del poligono PLACEHOLDER del backend) para probar el enlace
// LoRa en interiores. PRODUCCION: false -> sin fix no se transmite ni se acumula.
// ============================================================================
static const bool DEBUG_SEND_WITHOUT_FIX = true;
static const double DEBUG_TEST_LAT = 8.220000;
static const double DEBUG_TEST_LON = -72.270000;
static const uint32_t DEBUG_TEST_EPOCH_BASE = 1790640000;  // 2026-09-29 00:00:00 UTC

static const uint32_t ACK_TIMEOUT_MS = 2000;  // espera por cada intento
static const uint8_t MAX_RETRIES = 2;          // 1 intento + 2 reintentos

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
XPowersLibInterface *pmu = nullptr;
SX1276 radio = new Module(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, LORA_DIO1_PIN);

volatile bool radioIrq = false;
bool radioReady = false;
uint16_t nextSeq = 1;
uint32_t lastTxMs = 0;

String currentLine;
String lastGgaSentence;
uint32_t lastReportMs = 0;

// Diagnostico de antena (trama GSV): disponible antes del fix, a diferencia de los satelites en uso.
int satsInView = -1;
int satsWithSignal = 0;
int bestSnr = 0;
int cycleSatsWithSignal = 0;
int cycleBestSnr = 0;

void IRAM_ATTR onRadioIrq() {
    radioIrq = true;
}

// En el T-Beam ni el GPS ni el SX1276 tienen alimentacion directa: la entrega el PMU
// (LilyGO-LoRa-Series, LoRaBoards.cpp).
bool powerOnPeripherals() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    pmu = new XPowersAXP2101(Wire);
    if (pmu->init()) {
        Serial.println("PMU AXP2101 detectado (T-Beam v1.2)");
        pmu->setPowerChannelVoltage(XPOWERS_ALDO2, 3300);    // VDD del SX1276
        pmu->enablePowerOutput(XPOWERS_ALDO2);
        pmu->setPowerChannelVoltage(XPOWERS_ALDO3, 3300);    // VDD del GPS
        pmu->enablePowerOutput(XPOWERS_ALDO3);
        pmu->setPowerChannelVoltage(XPOWERS_VBACKUP, 3300);  // respaldo del RTC del GPS
        pmu->enablePowerOutput(XPOWERS_VBACKUP);
        return true;
    }
    delete pmu;

    pmu = new XPowersAXP192(Wire);
    if (pmu->init()) {
        Serial.println("PMU AXP192 detectado (T-Beam v1.0/v1.1)");
        pmu->setPowerChannelVoltage(XPOWERS_LDO2, 3300);     // VDD del SX1276
        pmu->enablePowerOutput(XPOWERS_LDO2);
        pmu->setPowerChannelVoltage(XPOWERS_LDO3, 3300);     // VDD del GPS
        pmu->enablePowerOutput(XPOWERS_LDO3);
        return true;
    }
    delete pmu;
    pmu = nullptr;
    return false;
}

// $GPGSV,totalMsgs,msgNum,satsInView,[prn,elev,azim,snr] x hasta 4 satelites*checksum
// Un ciclo completo son varias tramas (msgNum 1..totalMsgs); se publica al cerrar el ciclo.
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
            if (msgNum == 1) {
                cycleSatsWithSignal = 0;
                cycleBestSnr = 0;
            }
        } else if (fieldIndex == 3) {
            satsInView = field.toInt();
        } else if (fieldIndex >= 7 && (fieldIndex - 7) % 4 == 0) {
            int snr = field.toInt();
            if (snr > 0) {
                cycleSatsWithSignal++;
                if (snr > cycleBestSnr) cycleBestSnr = snr;
            }
        }

        fieldIndex++;
        start = comma + 1;
    }

    if (msgNum > 0 && msgNum == totalMsgs) {
        satsWithSignal = cycleSatsWithSignal;
        bestSnr = cycleBestSnr;
    }
}

void readGps() {
    while (GPSSerial.available() > 0) {
        char c = GPSSerial.read();
        gps.encode(c);

        if (c == '\n') {
            if (currentLine.startsWith("$GPGGA") || currentLine.startsWith("$GNGGA")) {
                lastGgaSentence = currentLine;
            } else if (currentLine.startsWith("$GPGSV")) {
                parseGsv(currentLine);
            }
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
            // Sin esto el buffer UART del GPS (256 bytes) se desborda durante la espera.
            readGps();
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

void transmitPosition() {
    Serial.println("========================================");
    if (!radioReady) {
        Serial.println("TX: radio LoRa no disponible, no se transmite");
        return;
    }
    bool realFix = hasValidFix() && isTimeKnown();
    if (!realFix && !DEBUG_SEND_WITHOUT_FIX) {
        Serial.println(hasValidFix() ? "TX: sin fecha/hora UTC, no se transmite" : "TX: sin fix, no se transmite");
        return;
    }

    DataPacket packet = {};
    packet.version = PROTOCOL_VERSION;
    packet.type = MSG_TYPE_DATA;
    packet.seq = nextSeq++;
    strncpy(packet.collarId, COLLAR_ID, COLLAR_ID_LEN);

    if (realFix) {
        packet.latE6 = (int32_t)lround(gps.location.lat() * 1e6);
        packet.lonE6 = (int32_t)lround(gps.location.lng() * 1e6);
        packet.timestamp = toEpochUtc(gps.date.year(), gps.date.month(), gps.date.day(),
                                      gps.time.hour(), gps.time.minute(), gps.time.second());
        uint32_t satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
        packet.satellites = (uint8_t)min<uint32_t>(satellites, 255);
    } else {
        packet.latE6 = (int32_t)lround(DEBUG_TEST_LAT * 1e6);
        packet.lonE6 = (int32_t)lround(DEBUG_TEST_LON * 1e6);
        packet.timestamp = DEBUG_TEST_EPOCH_BASE + millis() / 1000;
        packet.satellites = 0;
    }

    Serial.printf("TX%s seq=%u | %s | lat=%.6f lon=%.6f | ts=%lu | sats=%u\n",
                  realFix ? "" : " [PRUEBA SIN FIX]", packet.seq, COLLAR_ID, packet.latE6 / 1e6,
                  packet.lonE6 / 1e6, (unsigned long)packet.timestamp, packet.satellites);

    if (!sendWithAck(packet)) {
        Serial.printf("TX seq=%u: sin ACK tras %d intentos, se descarta\n", packet.seq, 1 + MAX_RETRIES);
    }
}

void printReport() {
    Serial.println("----------------------------------------");
    Serial.printf("NMEA -> caracteres: %lu | tramas OK: %lu | checksum fallido: %lu\n",
                  (unsigned long)gps.charsProcessed(),
                  (unsigned long)gps.passedChecksum(),
                  (unsigned long)gps.failedChecksum());

    if (gps.charsProcessed() == 0) {
        if (millis() > NO_DATA_WARNING_AFTER_MS) {
            Serial.println("ATENCION: no llega nada del GPS (revisar alimentacion del PMU, pines o baudios)");
        }
        return;
    }

    Serial.printf("Ultima trama GGA: %s\n",
                  lastGgaSentence.length() > 0 ? lastGgaSentence.c_str() : "(ninguna aun)");

    if (satsInView < 0) {
        Serial.println("Satelites a la vista: (sin trama GSV aun)");
    } else {
        Serial.printf("Satelites a la vista: %d | con senal: %d | mejor senal: %d dB-Hz\n",
                      satsInView, satsWithSignal, bestSnr);
    }

    if (isTimeKnown()) {
        Serial.printf("Hora UTC: %02d:%02d:%02d\n", gps.time.hour(), gps.time.minute(), gps.time.second());
    } else {
        Serial.println("Hora UTC: (aun no disponible)");
    }

    unsigned long satellites = gps.satellites.isValid() ? (unsigned long)gps.satellites.value() : 0;

    if (hasValidFix()) {
        Serial.printf("FIX: SI | lat: %.6f | lon: %.6f | satelites: %lu\n",
                      gps.location.lat(), gps.location.lng(), satellites);
    } else {
        Serial.printf("FIX: NO | satelites en uso: %lu\n", satellites);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("Collar IoT - Parte 2: GPS + transmision LoRa con ACK");
    Serial.printf("collar_id=%s | intervalo TX=%lu ms\n", COLLAR_ID, (unsigned long)TX_INTERVAL_MS);

    if (!powerOnPeripherals()) {
        Serial.println("ATENCION: no se detecto PMU (AXP2101/AXP192); GPS y LoRa podrian no estar alimentados");
    }

    GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    int16_t state = beginLoraRadio(radio);
    if (state == RADIOLIB_ERR_NONE) {
        radio.setPacketReceivedAction(onRadioIrq);
        radioReady = true;
        Serial.printf("LoRa SX1276 listo: %.1f MHz, SF%u, BW %.0f kHz, %d dBm\n", LORA_FREQUENCY_MHZ,
                      LORA_SPREADING_FACTOR, LORA_BANDWIDTH_KHZ, LORA_TX_POWER_DBM);
    } else {
        Serial.printf("ERROR: no se pudo iniciar el SX1276 (codigo %d)\n", state);
    }
}

void loop() {
    readGps();

    if (millis() - lastReportMs >= REPORT_INTERVAL_MS) {
        lastReportMs = millis();
        printReport();
    }

    if (millis() - lastTxMs >= TX_INTERVAL_MS) {
        lastTxMs = millis();
        transmitPosition();
    }
}
