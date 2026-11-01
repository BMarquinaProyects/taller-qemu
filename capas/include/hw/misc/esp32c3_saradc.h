/* ADC1 APB_SARADC del ESP32-C3 para Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"

#define TYPE_ESP32C3_SARADC "esp32c3.saradc"
#define ESP32C3_SARADC(obj) OBJECT_CHECK(ESP32C3SarAdcState, (obj), TYPE_ESP32C3_SARADC)
#define ESP32C3_SARADC_CANALES 5
#define ESP32C3_SARADC_REGS_SIZE 0x400

typedef struct ESP32C3SarAdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t registros[ESP32C3_SARADC_REGS_SIZE / 4];
    uint16_t valores[ESP32C3_SARADC_CANALES];
} ESP32C3SarAdcState;

void esp32c3_saradc_fijar_valor(ESP32C3SarAdcState *s, uint8_t gpio, uint16_t valor);
