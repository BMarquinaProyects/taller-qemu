/* GPIO S3 de Taller. Copyright (c) 2026 Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "qemu/timer.h"
#include "hw/misc/taller_armado.h"

#define TYPE_ESP32S3_GPIO "esp32s3.gpio"
#define ESP32S3_GPIO(obj) OBJECT_CHECK(ESP32S3GPIOState, (obj), TYPE_ESP32S3_GPIO)
/* Índices 0..48; GPIO22..25 no existen físicamente. */
#define ESP32S3_GPIO_PINES 49
#define ESP32S3_GPIO_MASCARA 0x1fffffc3fffffull
#define ESP32S3_GPIO_SENAL_SOFTWARE 256
#define ESP32S3_GPIO_PAD_DRIVER (1u << 2)
#define ESP32S3_STRAP_MODE_FLASH_BOOT 0x4

/* soc/esp32s3/register/soc/gpio_reg.h del core Arduino 3.3.12. */
REG32(ESP32S3_GPIO_OUT, 0x004)
REG32(ESP32S3_GPIO_OUT_W1TS, 0x008)
REG32(ESP32S3_GPIO_OUT_W1TC, 0x00c)
REG32(ESP32S3_GPIO_OUT1, 0x010)
REG32(ESP32S3_GPIO_OUT1_W1TS, 0x014)
REG32(ESP32S3_GPIO_OUT1_W1TC, 0x018)
REG32(ESP32S3_GPIO_ENABLE, 0x020)
REG32(ESP32S3_GPIO_ENABLE_W1TS, 0x024)
REG32(ESP32S3_GPIO_ENABLE_W1TC, 0x028)
REG32(ESP32S3_GPIO_ENABLE1, 0x02c)
REG32(ESP32S3_GPIO_ENABLE1_W1TS, 0x030)
REG32(ESP32S3_GPIO_ENABLE1_W1TC, 0x034)
REG32(ESP32S3_GPIO_STRAP, 0x038)
REG32(ESP32S3_GPIO_IN, 0x03c)
REG32(ESP32S3_GPIO_IN1, 0x040)
REG32(ESP32S3_GPIO_PIN0, 0x074)
REG32(ESP32S3_GPIO_FUNC0_OUT_SEL_CFG, 0x554)

typedef struct ESP32S3GpioTren {
    QEMUTimer *timer;
    struct ESP32S3GPIOState *gpio;
    unsigned indice;
} ESP32S3GpioTren;

typedef struct ESP32S3GPIOState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t strap_mode;
    uint64_t out;
    uint64_t enable;
    uint64_t in_externo;
    uint64_t drenador_abierto;
    uint32_t registro_pin[ESP32S3_GPIO_PINES];
    uint32_t seleccion_salida[ESP32S3_GPIO_PINES];
    TallerArmado armado;
    ESP32S3GpioTren trenes[TALLER_ARMADO_RANURAS];
} ESP32S3GPIOState;

void esp32s3_gpio_fijar_entrada(ESP32S3GPIOState *s, uint8_t pin, bool nivel);
void esp32s3_gpio_armar_entrada(ESP32S3GPIOState *s, const uint8_t *datos, size_t largo);
int esp32s3_gpio_pin_de_senal(void *opaque, int senal);
