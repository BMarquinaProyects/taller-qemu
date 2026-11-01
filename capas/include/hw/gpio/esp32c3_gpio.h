/*
 * GPIO del ESP32-C3 para Taller, basado en el modelo de Espressif.
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "qemu/timer.h"
#include "hw/misc/taller_armado.h"

#define TYPE_ESP32C3_GPIO "esp32c3.gpio"
#define ESP32C3_GPIO(obj) OBJECT_CHECK(ESP32C3GPIOState, (obj), TYPE_ESP32C3_GPIO)

#define ESP32C3_GPIO_PINES 22
#define ESP32C3_GPIO_MASCARA ((1u << ESP32C3_GPIO_PINES) - 1)
#define ESP32C3_GPIO_SENAL_SOFTWARE 128
#define ESP32C3_GPIO_PAD_DRIVER (1u << 2)

#define ESP32C3_STRAP_MODE_FLASH_BOOT 0x8
#define ESP32C3_STRAP_MODE_UART_BOOT  0x2
#define ESP32C3_STRAP_MODE_USB_BOOT   0x0

/* soc/esp32c3/register/soc/gpio_reg.h del core Arduino 3.3.12. */
REG32(ESP32C3_GPIO_OUT, 0x004)
REG32(ESP32C3_GPIO_OUT_W1TS, 0x008)
REG32(ESP32C3_GPIO_OUT_W1TC, 0x00c)
REG32(ESP32C3_GPIO_ENABLE, 0x020)
REG32(ESP32C3_GPIO_ENABLE_W1TS, 0x024)
REG32(ESP32C3_GPIO_ENABLE_W1TC, 0x028)
REG32(ESP32C3_GPIO_STRAP, 0x038)
REG32(ESP32C3_GPIO_IN, 0x03c)
REG32(ESP32C3_GPIO_PIN0, 0x074)
REG32(ESP32C3_GPIO_FUNC0_OUT_SEL_CFG, 0x554)

typedef struct ESP32C3GpioTren {
    QEMUTimer *timer;
    struct ESP32C3GPIOState *gpio;
    unsigned indice;
} ESP32C3GpioTren;

typedef struct ESP32C3GPIOState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t strap_mode;
    uint32_t out;
    uint32_t enable;
    uint32_t in_externo;
    uint32_t drenador_abierto;
    uint32_t registro_pin[ESP32C3_GPIO_PINES];
    uint32_t seleccion_salida[ESP32C3_GPIO_PINES];
    TallerArmado armado;
    ESP32C3GpioTren trenes[TALLER_ARMADO_RANURAS];
} ESP32C3GPIOState;

void esp32c3_gpio_fijar_entrada(ESP32C3GPIOState *s, uint8_t pin, bool nivel);
void esp32c3_gpio_armar_entrada(ESP32C3GPIOState *s, const uint8_t *datos,
                               size_t largo);
