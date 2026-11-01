/* Maestro I2C0 del ESP32-C3 para Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"

#define TYPE_ESP32C3_I2C "esp32c3.i2c"
#define ESP32C3_I2C(obj) OBJECT_CHECK(ESP32C3I2CState, (obj), TYPE_ESP32C3_I2C)
#define ESP32C3_I2C_REGS_SIZE 0x1000
#define ESP32C3_I2C_FIFO 32
#define ESP32C3_I2C_COMANDOS 8

typedef struct ESP32C3I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    uint32_t registros[0x100 / 4];
    uint8_t tx[ESP32C3_I2C_FIFO], rx[ESP32C3_I2C_FIFO];
    unsigned tx_lectura, tx_escritura, tx_cantidad;
    unsigned rx_lectura, rx_escritura, rx_cantidad;
    unsigned comando, bytes_hechos;
    bool ejecutando, ocupado, espera_direccion, respuesta_nack;
} ESP32C3I2CState;
