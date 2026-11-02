/* SPI2 (GPSPI2) compartido C3/S3 para Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"

#define TYPE_ESP32C3_SPI2 "esp32c3.spi2"
#define ESP32C3_SPI2(obj) OBJECT_CHECK(ESP32C3Spi2State, (obj), TYPE_ESP32C3_SPI2)
#define ESP32C3_SPI2_REGS_SIZE 0x1000
#define ESP32C3_SPI2_BUS 2

typedef struct ESP32C3Spi2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *spi;
    ESPGdmaState *gdma;
    bool es_s3;
    unsigned canales_dma;
    uint32_t sram_inicio, sram_fin;
    uint32_t registros[0x100 / 4];
} ESP32C3Spi2State;
