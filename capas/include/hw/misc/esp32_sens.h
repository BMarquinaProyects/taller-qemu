/*
 * ADC1 del ESP32 por el controlador RTC (SENS).
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "hw/sysbus.h"

#define TYPE_ESP32_SENS "esp32.sens"
#define ESP32_SENS_REGS_SIZE 0x400
#define ESP32_SENS_CANALES_ADC1 8

typedef struct Esp32SensState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t registros[ESP32_SENS_REGS_SIZE / 4];
    uint16_t valores[ESP32_SENS_CANALES_ADC1];
} Esp32SensState;

OBJECT_DECLARE_SIMPLE_TYPE(Esp32SensState, ESP32_SENS)

/* Canal ADC1 (0..7); los valores superiores a 4095 se saturan. */
void esp32_sens_fijar_valor(int canal, uint16_t valor);
