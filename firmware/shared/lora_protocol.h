#pragma once

// Protocolo y configuracion de radio compartidos por el collar y la estacion base.
// Ambos firmwares incluyen este mismo archivo (build_flags = -I ../shared) para que
// las estructuras y los parametros de radio nunca se desincronicen entre las placas.

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// --- Pines del SX1276 en el T-Beam (LilyGO-LoRa-Series, utilities.h, bloque T-Beam) ---
static const int LORA_SCK_PIN = 5;
static const int LORA_MISO_PIN = 19;
static const int LORA_MOSI_PIN = 27;
static const int LORA_CS_PIN = 18;
static const int LORA_RST_PIN = 23;
static const int LORA_DIO0_PIN = 26;
static const int LORA_DIO1_PIN = 33;

// --- Parametros de radio: deben ser identicos en collar y estacion base ---
static const float LORA_FREQUENCY_MHZ = 915.0;
static const float LORA_BANDWIDTH_KHZ = 125.0;
static const uint8_t LORA_SPREADING_FACTOR = 9;
static const uint8_t LORA_CODING_RATE = 5;          // 4/5
static const uint8_t LORA_SYNC_WORD = 0x12;         // privado; LoRaWAN usa 0x34
static const uint16_t LORA_PREAMBLE_LENGTH = 8;
// 10 dBm para pruebas de escritorio (placas a ~1 m; a maxima potencia el receptor se satura).
// En campo subir a 17-20 dBm.
static const int8_t LORA_TX_POWER_DBM = 10;

// --- Protocolo ---
static const uint8_t PROTOCOL_VERSION = 1;
static const uint8_t MSG_TYPE_DATA = 0x01;
static const uint8_t MSG_TYPE_ACK = 0x02;
static const size_t COLLAR_ID_LEN = 16;

// Ambos extremos son ESP32 (little-endian), por eso se transmite el struct empaquetado tal cual.
struct __attribute__((packed)) DataPacket {
    uint8_t version;
    uint8_t type;                  // MSG_TYPE_DATA
    uint16_t seq;                  // igual en los reintentos del mismo paquete
    char collarId[COLLAR_ID_LEN];  // rellenado con ceros, sin terminador si ocupa los 16
    int32_t latE6;                 // grados * 1e6
    int32_t lonE6;                 // grados * 1e6
    uint32_t timestamp;            // epoch UTC en segundos (fecha/hora del GPS)
    uint8_t satellites;            // satelites en uso
};

struct __attribute__((packed)) AckPacket {
    uint8_t version;
    uint8_t type;                  // MSG_TYPE_ACK
    uint16_t seq;                  // seq del DataPacket que se confirma
    char collarId[COLLAR_ID_LEN];  // collar al que va dirigido
};

static_assert(sizeof(DataPacket) == 33, "DataPacket debe medir 33 bytes");
static_assert(sizeof(AckPacket) == 20, "AckPacket debe medir 20 bytes");

inline int16_t beginLoraRadio(SX1276 &radio) {
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    int16_t state = radio.begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR,
                                LORA_CODING_RATE, LORA_SYNC_WORD, LORA_TX_POWER_DBM,
                                LORA_PREAMBLE_LENGTH);
    if (state != RADIOLIB_ERR_NONE) {
        return state;
    }
    return radio.setCRC(true);
}
