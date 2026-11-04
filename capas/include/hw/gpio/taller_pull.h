/* Contactos pasivos de Taller. Copyright (c) 2026 Taller. GPL-2.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TALLER_IOMUX_BYTES 0x2000
#define TALLER_RTCIO_BYTES 0x400
#define TALLER_FUN_PU (1u << 8)
#define TALLER_FUN_PD (1u << 7)

/* GPIO_PIN_MUX_REG / GPIO_PIN_MUX_REG_OFFSET de ESP-IDF 5.5.6, ESP32.
 * Cero marca los GPIO sin pad. El orden físico NO es el de los GPIO. */
static const uint8_t taller_esp32_mux[40] = {
    0x44, 0x88, 0x40, 0x84, 0x48, 0x6c, 0x60, 0x64,
    0x68, 0x54, 0x58, 0x5c, 0x34, 0x38, 0x30, 0x3c,
    0x4c, 0x50, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x8c,
    0, 0x24, 0x28, 0x2c, 0, 0, 0, 0,
    0x1c, 0x20, 0x14, 0x18, 0x04, 0x08, 0x0c, 0x10
};

typedef struct TallerPullRtc {
    uint8_t pin, reg, arriba, abajo;
} TallerPullRtc;

/* rtc_io_desc: offset y posiciones de pullup/pulldown. GPIO34..39 no
 * aparecen porque sus dos máscaras son cero: carecen de resistencias. */
static const TallerPullRtc taller_esp32_rtc[] = {
    {25, 0x84, 27, 28}, {26, 0x88, 27, 28},
    {33, 0x8c, 27, 28}, {32, 0x8c, 22, 23},
    {4, 0x94, 27, 28}, {0, 0x98, 27, 28}, {2, 0x9c, 27, 28},
    {15, 0xa0, 27, 28}, {13, 0xa4, 27, 28}, {12, 0xa8, 27, 28},
    {14, 0xac, 27, 28}, {27, 0xb0, 27, 28}
};

static inline void taller_pull_mascaras(const uint32_t *iomux,
                                       const uint32_t *rtcio, unsigned pines,
                                       uint64_t validos, uint64_t *arriba,
                                       uint64_t *abajo)
{
    *arriba = *abajo = 0;
    for (unsigned pin = 0; pin < pines; pin++) {
        unsigned reg = rtcio ? taller_esp32_mux[pin] : 4 + 4 * pin;
        uint64_t bit = 1ull << pin;
        /* El clásico tiene pads de entrada sin pull también en IO_MUX. */
        if (!reg || !(validos & bit) || (rtcio && pin >= 34)) {
            continue;
        }
        if (iomux[reg / 4] & TALLER_FUN_PU) {
            *arriba |= bit;
        }
        if (iomux[reg / 4] & TALLER_FUN_PD) {
            *abajo |= bit;
        }
    }
    if (rtcio) {
        for (unsigned i = 0; i < sizeof(taller_esp32_rtc) /
                                 sizeof(taller_esp32_rtc[0]); i++) {
            const TallerPullRtc *r = &taller_esp32_rtc[i];
            uint32_t valor = rtcio[r->reg / 4];
            if (valor & (1u << r->arriba)) {
                *arriba |= 1ull << r->pin;
            }
            if (valor & (1u << r->abajo)) {
                *abajo |= 1ull << r->pin;
            }
        }
    }
}

static inline uint64_t taller_pull_resolver(uint64_t anterior, uint64_t sueltos,
                                           uint64_t arriba, uint64_t abajo)
{
    /* Ambos activos: gana arriba (silicio indefinido; PU algo más fuerte).
     * Sin pull: conservar carga del pad, simplificación determinista. */
    return (anterior | (sueltos & arriba)) & ~(sueltos & abajo & ~arriba);
}
