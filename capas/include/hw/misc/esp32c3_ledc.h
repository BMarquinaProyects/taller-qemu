/* LEDC del ESP32-C3 para Taller. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"

#define TYPE_ESP32C3_LEDC "esp32c3.ledc"
#define ESP32C3_LEDC(obj) OBJECT_CHECK(ESP32C3LEDCState, (obj), TYPE_ESP32C3_LEDC)
#define ESP32C3_LEDC_CANALES 6
#define ESP32C3_LEDC_TIMERS 4
#define ESP32C3_LEDC_SENAL0 45
#define ESP32C3_LEDC_REGS_SIZE 0x100

typedef struct ESP32C3LEDCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    struct ESP32C3GPIOState *gpio;
    uint32_t registros[ESP32C3_LEDC_REGS_SIZE / 4];
    bool pwm_emitido[ESP32C3_LEDC_CANALES];
    int pwm_gpio[ESP32C3_LEDC_CANALES];
    uint32_t pwm_frecuencia_mhz[ESP32C3_LEDC_CANALES];
    uint32_t pwm_ciclo_ppm[ESP32C3_LEDC_CANALES];
} ESP32C3LEDCState;

void esp32c3_ledc_actualizar_senal(int senal);
