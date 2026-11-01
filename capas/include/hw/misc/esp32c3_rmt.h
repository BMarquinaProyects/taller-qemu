/* RMT TX del ESP32-C3 para Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_ESP32C3_RMT "esp32c3.rmt"
#define ESP32C3_RMT(obj) OBJECT_CHECK(ESP32C3RMTState, (obj), TYPE_ESP32C3_RMT)
#define ESP32C3_RMT_CANALES_TX 2
#define ESP32C3_RMT_PALABRAS_BLOQUE 48
#define ESP32C3_RMT_PALABRAS 192
#define ESP32C3_RMT_SENAL0 51
#define ESP32C3_RMT_REGS_SIZE 0x1000
#define ESP32C3_RMT_RAM 0x400

typedef struct ESP32C3RMTState ESP32C3RMTState;

typedef struct ESP32C3RMTCanal {
    ESP32C3RMTState *rmt;
    unsigned numero;
    QEMUTimer *timer;
    GArray *simbolos;
    unsigned posicion;
    unsigned desde_umbral;
    uint32_t tick_ps;
    uint64_t tiempo_ps;
    unsigned ticks_pendientes;
    bool fin_pendiente;
    bool activo;
    bool esperando_isr;
    uint8_t gpio;
    int64_t inicio_ns;
    int64_t pausa_desde_ns;
    int64_t desfase_ns;
} ESP32C3RMTCanal;

struct ESP32C3RMTState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    struct ESP32C3GPIOState *gpio;
    uint32_t registros[0x100 / 4];
    uint32_t memoria[ESP32C3_RMT_PALABRAS];
    unsigned apb_lectura[4];
    unsigned apb_escritura[4];
    ESP32C3RMTCanal canal[ESP32C3_RMT_CANALES_TX];
};
