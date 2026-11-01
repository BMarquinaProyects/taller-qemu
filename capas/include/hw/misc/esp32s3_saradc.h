/* ADC1 SENS del ESP32-S3 para Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"

#define TYPE_ESP32S3_SARADC "esp32s3.saradc"
#define ESP32S3_SARADC(obj) OBJECT_CHECK(ESP32S3SarAdcState, (obj), TYPE_ESP32S3_SARADC)
#define ESP32S3_SARADC_CANALES 10
#define ESP32S3_SARADC_REGS_SIZE 0x400

typedef struct ESP32S3SarAdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t registros[ESP32S3_SARADC_REGS_SIZE / 4];
    uint16_t valores[ESP32S3_SARADC_CANALES];
} ESP32S3SarAdcState;

void esp32s3_saradc_fijar_valor(ESP32S3SarAdcState *s, uint8_t gpio, uint16_t valor);
