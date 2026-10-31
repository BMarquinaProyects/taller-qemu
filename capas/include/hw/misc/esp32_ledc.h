#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/misc/led.h"

#define TYPE_ESP32_LEDC "misc.esp32.ledc"
#define ESP32_LEDC(obj) OBJECT_CHECK(Esp32LEDCState, (obj), TYPE_ESP32_LEDC)
#define ESP32_LEDC_TIMER_CNT 8
#define ESP32_LEDC_CHANNEL_CNT 16

/* Índices de gpio_sig_map.h del ESP32 clásico; cada grupo tiene ocho señales. */
#define ESP32_LEDC_SENAL_HS0 71
#define ESP32_LEDC_SENAL_LS0 79

typedef struct Esp32LEDCState {
    SysBusDevice parent_object;
    MemoryRegion iomem;
    uint32_t duty_res[ESP32_LEDC_TIMER_CNT];
    uint32_t timer_conf_reg[ESP32_LEDC_TIMER_CNT];
    uint32_t channel_conf0_reg[ESP32_LEDC_CHANNEL_CNT];
    uint32_t timer_valor[ESP32_LEDC_TIMER_CNT];
    uint32_t canal_hpoint[ESP32_LEDC_CHANNEL_CNT];
    uint32_t canal_duty[ESP32_LEDC_CHANNEL_CNT];
    uint32_t canal_conf1[ESP32_LEDC_CHANNEL_CNT];
    uint32_t canal_duty_leido[ESP32_LEDC_CHANNEL_CNT];
    uint32_t registros_comunes[5]; /* INT_RAW, INT_ST, INT_ENA, INT_CLR, CONF */
    bool pwm_emitido[ESP32_LEDC_CHANNEL_CNT];
    int pwm_gpio[ESP32_LEDC_CHANNEL_CNT];
    uint32_t pwm_frecuencia_mhz[ESP32_LEDC_CHANNEL_CNT];
    uint32_t pwm_ciclo_ppm[ESP32_LEDC_CHANNEL_CNT];
    LEDState led[ESP32_LEDC_CHANNEL_CNT];
} Esp32LEDCState;

/* La matriz GPIO avisa tanto de la señal desconectada como de la conectada. */
void esp32_ledc_actualizar_senal(int senal);

REG32(LEDC_CONF_REG, 0x190)

#define LEDC_REG_GROUP(name, base) \
    REG32(name ## _CONF0_REG, (base)) \
        FIELD(name ## _CONF0_REG, TIMER_SEL, 0, 2) \
        FIELD(name ## _CONF0_REG, SIG_OUT_EN, 2, 1) \
        FIELD(name ## _CONF0_REG, PARA_UP, 4, 1) \
    REG32(name ## _HPOINT_REG, ((base) + 0x004)) \
    REG32(name ## _CONF1_REG, ((base) + 0x00C)) \
        FIELD(name ## _CONF1_REG, DUTY_START, 31, 1) \
    REG32(name ## _DUTY_REG, ((base) + 0x008)) \
        FIELD(name ## _DUTY_REG, DUTY, 0, 25) \
    REG32(name ## _DUTY_R_REG, ((base) + 0x010))

#define LEDC_TIMER_REG_GROUP(name, base) \
    REG32(name ## _CONF_REG, (base)) \
        FIELD(name ## _CONF_REG, DUTY_RES, 0, 5) \
        FIELD(name ## _CONF_REG, DIV_NUM, 5, 18) \
        FIELD(name ## _CONF_REG, TICK_SEL, 25, 1) \
    REG32(name ## _VALUE_REG, ((base) + 0x004))

LEDC_REG_GROUP(LEDC_HSCH0, 0x000)
LEDC_REG_GROUP(LEDC_HSCH1, 0x014)
LEDC_REG_GROUP(LEDC_HSCH2, 0x028)
LEDC_REG_GROUP(LEDC_HSCH3, 0x03C)
LEDC_REG_GROUP(LEDC_HSCH4, 0x050)
LEDC_REG_GROUP(LEDC_HSCH5, 0x064)
LEDC_REG_GROUP(LEDC_HSCH6, 0x078)
LEDC_REG_GROUP(LEDC_HSCH7, 0x08C)

LEDC_REG_GROUP(LEDC_LSCH0, 0x0A0)
LEDC_REG_GROUP(LEDC_LSCH1, 0x0B4)
LEDC_REG_GROUP(LEDC_LSCH2, 0x0C8)
LEDC_REG_GROUP(LEDC_LSCH3, 0x0DC)
LEDC_REG_GROUP(LEDC_LSCH4, 0x0F0)
LEDC_REG_GROUP(LEDC_LSCH5, 0x104)
LEDC_REG_GROUP(LEDC_LSCH6, 0x118)
LEDC_REG_GROUP(LEDC_LSCH7, 0x12C)

LEDC_TIMER_REG_GROUP(LEDC_HSTIMER0, 0x140)
LEDC_TIMER_REG_GROUP(LEDC_HSTIMER1, 0x148)
LEDC_TIMER_REG_GROUP(LEDC_HSTIMER2, 0x150)
LEDC_TIMER_REG_GROUP(LEDC_HSTIMER3, 0x158)

LEDC_TIMER_REG_GROUP(LEDC_LSTIMER0, 0x160)
LEDC_TIMER_REG_GROUP(LEDC_LSTIMER1, 0x168)
LEDC_TIMER_REG_GROUP(LEDC_LSTIMER2, 0x170)
LEDC_TIMER_REG_GROUP(LEDC_LSTIMER3, 0x178)

REG32(LEDC_INT_RAW_REG, 0x180)
REG32(LEDC_INT_ST_REG, 0x184)
REG32(LEDC_INT_ENA_REG, 0x188)
REG32(LEDC_INT_CLR_REG, 0x18C)
