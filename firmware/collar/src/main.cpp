#include <Arduino.h>
#include <Wire.h>
#include <TinyGPS++.h>
#include <XPowersLib.h>

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

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
XPowersLibInterface *pmu = nullptr;

String currentLine;
String lastGgaSentence;
uint32_t lastReportMs = 0;

// Diagnostico de antena (trama GSV): disponible antes del fix, a diferencia de los satelites en uso.
int satsInView = -1;
int satsWithSignal = 0;
int bestSnr = 0;
int cycleSatsWithSignal = 0;
int cycleBestSnr = 0;

// En el T-Beam el GPS no tiene alimentacion directa: la entrega el PMU.
bool powerOnGps() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    pmu = new XPowersAXP2101(Wire);
    if (pmu->init()) {
        Serial.println("PMU AXP2101 detectado (T-Beam v1.2)");
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

    // Con el campo NMEA vacio TinyGPS++ marca la hora como valida en 00:00:00; una fecha 0 nunca es real.
    bool timeKnown = gps.time.isValid() && gps.date.isValid() && gps.date.value() != 0;
    if (timeKnown) {
        Serial.printf("Hora UTC: %02d:%02d:%02d\n", gps.time.hour(), gps.time.minute(), gps.time.second());
    } else {
        Serial.println("Hora UTC: (aun no disponible)");
    }

    unsigned long satellites = gps.satellites.isValid() ? (unsigned long)gps.satellites.value() : 0;
    bool hasFix = gps.location.isValid() && gps.location.age() < FIX_MAX_AGE_MS;

    if (hasFix) {
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
    Serial.println("Collar IoT - Parte 1: lectura de GPS");

    if (!powerOnGps()) {
        Serial.println("ATENCION: no se detecto PMU (AXP2101/AXP192); el GPS podria no estar alimentado");
    }

    GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void loop() {
    readGps();

    if (millis() - lastReportMs >= REPORT_INTERVAL_MS) {
        lastReportMs = millis();
        printReport();
    }
}
