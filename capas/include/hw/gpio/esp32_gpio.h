#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"
#include "qemu/timer.h"
#include "hw/misc/taller_armado.h"

#define TYPE_ESP32_GPIO "esp32.gpio"
#define ESP32_GPIO(obj)             OBJECT_CHECK(Esp32GpioState, (obj), TYPE_ESP32_GPIO)
#define ESP32_GPIO_GET_CLASS(obj)   OBJECT_GET_CLASS(Esp32GpioClass, obj, TYPE_ESP32_GPIO)
#define ESP32_GPIO_CLASS(klass)     OBJECT_CLASS_CHECK(Esp32GpioClass, klass, TYPE_ESP32_GPIO)

/* Registros de salida/habilitación/entrada (TRM del ESP32, cap. GPIO). Taller. */
REG32(GPIO_OUT, 0x0004)
REG32(GPIO_OUT_W1TS, 0x0008)
REG32(GPIO_OUT_W1TC, 0x000C)
REG32(GPIO_OUT1, 0x0010)
REG32(GPIO_OUT1_W1TS, 0x0014)
REG32(GPIO_OUT1_W1TC, 0x0018)
REG32(GPIO_ENABLE, 0x0020)
REG32(GPIO_ENABLE_W1TS, 0x0024)
REG32(GPIO_ENABLE_W1TC, 0x0028)
REG32(GPIO_ENABLE1, 0x002C)
REG32(GPIO_ENABLE1_W1TS, 0x0030)
REG32(GPIO_ENABLE1_W1TC, 0x0034)
REG32(GPIO_STRAP, 0x0038)
REG32(GPIO_IN, 0x003C)
REG32(GPIO_IN1, 0x0040)
REG32(GPIO_FUNC0_OUT_SEL_CFG, 0x0530)
    FIELD(GPIO_FUNC0_OUT_SEL_CFG, OUT_SEL, 0, 9)

#define ESP32_GPIO_CANTIDAD_PINES 40
#define ESP32_GPIO_SENAL_SOFTWARE 256

/* GPIO 0-31 en los registros "sin 1"; GPIO 32-39 en los bits 0-7 de los registros "1". */
#define ESP32_GPIO_MASCARA_ALTA 0xFFull

#define ESP32_STRAP_MODE_FLASH_BOOT 0x12
#define ESP32_STRAP_MODE_UART_BOOT  0x0f

typedef struct Esp32GpioTemporizadorArmado {
    QEMUTimer *timer;
    struct Esp32GpioState *gpio;
    unsigned indice;
} Esp32GpioTemporizadorArmado;

typedef struct Esp32GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t strap_mode;
    /* Taller: bit n = GPIO n (0-39) */
    uint64_t out;        /* nivel que el firmware escribe */
    uint64_t enable;     /* 1 = el pin es salida */
    uint64_t in_externo; /* nivel que llega desde fuera a los pines de entrada (por defecto, todo a 1) */
    uint32_t seleccion_salida[ESP32_GPIO_CANTIDAD_PINES];
    TallerArmado armado;
    Esp32GpioTemporizadorArmado trenes[TALLER_ARMADO_RANURAS];
} Esp32GpioState;

/* Taller: nivel que llega desde fuera a un pin de entrada (botones, sensores…). */
void esp32_gpio_fijar_entrada(uint8_t pin, bool nivel);
/* Carga ENTRADA_ARMADA ya separada de su cabecera; ignora cargas inválidas. */
void esp32_gpio_armar_entrada(const uint8_t *datos, size_t largo);
/* Primer pin conectado a la señal de la matriz de salida, o -1. */
int esp32_gpio_pin_de_senal(int senal);

typedef struct Esp32GpioClass {
    SysBusDeviceClass parent_class;
} Esp32GpioClass;
