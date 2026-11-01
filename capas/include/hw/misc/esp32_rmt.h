/* Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later. */
#pragma once

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "hw/misc/taller_rmt.h"

#define TYPE_ESP32_RMT "esp32.rmt"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32RmtState, ESP32_RMT)

typedef struct Esp32RmtCanal {
    Esp32RmtState *rmt;
    unsigned numero;
    QEMUTimer *timer;
    TallerRmtPaso pendiente;
    GArray *simbolos;
    int64_t inicio_ns;
    uint8_t gpio;
    /* Contrapresión (ver esp32_rmt_timer): parado tras tx_thr_event hasta que la ISR lo limpie. */
    bool esperando_isr;
    int64_t pausa_desde_ns;
    int64_t desfase_ns; /* tiempo virtual total en pausa durante este tren */
} Esp32RmtCanal;

struct Esp32RmtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    TallerRmt logica;
    Esp32RmtCanal canal[TALLER_RMT_CANALES];
};
