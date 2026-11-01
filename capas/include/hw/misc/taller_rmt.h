/*
 * RMT del ESP32 clásico: registros y avance TX sin dependencias de QEMU.
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TALLER_RMT_CANALES 8
#define TALLER_RMT_PALABRAS 512
#define TALLER_RMT_FIN 1u
#define TALLER_RMT_UMBRAL 2u
#define TALLER_RMT_VUELTA 4u

typedef struct TallerRmtTx {
    unsigned posicion;
    unsigned desde_umbral;
    uint32_t tick_ps;
    uint64_t tiempo_ps;
    bool activo;
} TallerRmtTx;

typedef struct TallerRmtPaso {
    uint32_t simbolo;
    unsigned ticks;
    bool fin;
    bool valido;
} TallerRmtPaso;

typedef struct TallerRmt {
    uint32_t registros[64];
    uint32_t memoria[TALLER_RMT_PALABRAS];
    TallerRmtTx tx[TALLER_RMT_CANALES];
    unsigned apb_lectura[TALLER_RMT_CANALES];
    unsigned apb_escritura[TALLER_RMT_CANALES];
} TallerRmt;

static inline uint32_t taller_rmt_tick_ps(unsigned divisor, bool apb)
{
    return (divisor ? divisor : 256) * (apb ? 12500u : 1000000u);
}

static inline unsigned taller_rmt_capacidad(const TallerRmt *s, unsigned canal)
{
    unsigned bloques = (s->registros[(0x20 + canal * 8) / 4] >> 24) & 15;
    /* No permitir que una configuración inválida salga de las 512 palabras. */
    if (bloques > TALLER_RMT_CANALES - canal) {
        bloques = TALLER_RMT_CANALES - canal;
    }
    return bloques * 64;
}

static inline void taller_rmt_iniciar(TallerRmt *s)
{
    memset(s, 0, sizeof(*s));
    for (unsigned c = 0; c < TALLER_RMT_CANALES; c++) {
        s->registros[(0x20 + c * 8) / 4] = 0x31100002;
        s->registros[(0x24 + c * 8) / 4] = 0x00000f20;
        s->registros[(0xb0 + c * 4) / 4] = 0x00400040;
        s->registros[(0xd0 + c * 4) / 4] = 128;
    }
    s->registros[0xfc / 4] = 0x16022600;
}

static inline uint32_t taller_rmt_leer(TallerRmt *s, unsigned direccion)
{
    if (direccion >= 0x800 && direccion < 0x1000) {
        return (s->registros[0xf0 / 4] & 1) ? s->memoria[(direccion - 0x800) / 4] : 0;
    }
    if (direccion < 0x20) {
        if (s->registros[0xf0 / 4] & 1) {
            return 0;
        }
        unsigned c = direccion / 4;
        unsigned p = s->apb_lectura[c]++;
        return p < taller_rmt_capacidad(s, c) ? s->memoria[c * 64 + p] : 0;
    }
    if (direccion >= 0x60 && direccion < 0x80) {
        unsigned c = (direccion - 0x60) / 4;
        return ((c * 64 + s->tx[c].posicion) << 12) |
               (s->tx[c].activo ? 1u << 24 : 0) |
               (s->tx[c].posicion >= taller_rmt_capacidad(s, c) ? 1u << 29 : 0);
    }
    if (direccion >= 0x80 && direccion < 0xa0) {
        unsigned c = (direccion - 0x80) / 4;
        return (s->apb_lectura[c] << 16) | s->apb_escritura[c];
    }
    if (direccion == 0xa4) {
        return s->registros[0xa0 / 4] & s->registros[0xa8 / 4];
    }
    if (direccion == 0xac || direccion >= 0x100) {
        return 0;
    }
    return s->registros[direccion / 4];
}

/* Devuelve máscara de canales iniciados; los pulsos de control se autocancelan. */
static inline unsigned taller_rmt_escribir(TallerRmt *s, unsigned direccion,
                                           uint32_t valor)
{
    if (direccion >= 0x800 && direccion < 0x1000) {
        if (s->registros[0xf0 / 4] & 1) {
            s->memoria[(direccion - 0x800) / 4] = valor;
        }
    } else if (direccion < 0x20) {
        if (s->registros[0xf0 / 4] & 1) {
            return 0;
        }
        unsigned c = direccion / 4;
        unsigned p = s->apb_escritura[c]++;
        if (p < taller_rmt_capacidad(s, c)) {
            s->memoria[c * 64 + p] = valor;
        }
    } else if (direccion == 0xac) {
        s->registros[0xa0 / 4] &= ~valor;
    } else if (direccion >= 0x20 && direccion < 0x60 && (direccion & 4)) {
        unsigned c = (direccion - 0x24) / 8;
        TallerRmtTx *tx = &s->tx[c];
        s->registros[direccion / 4] = valor & ~0x1001du;
        if (valor & 8) { /* MEM_RD_RST, independiente de TX_START. */
            tx->posicion = 0;
            tx->desde_umbral = 0;
        }
        if (valor & 16) {
            s->apb_lectura[c] = s->apb_escritura[c] = 0;
        }
        if (valor & 1) {
            tx->desde_umbral = 0;
            tx->tiempo_ps = 0;
            tx->tick_ps = taller_rmt_tick_ps(s->registros[(direccion - 4) / 4] & 255,
                                            (valor & (1u << 17)) != 0);
            /*
             * MEM_OWNER (bit 5) solo decide quién escribe la RAM al RECIBIR. El driver de
             * ESP-IDF 5.5 lo deja a 1 (valor de reinicio) en los canales de TX y el chip
             * transmite igual: no puede bloquear el envío (con esa comprobación, NeoPixel
             * se colgaba en show()).
             */
            tx->activo = taller_rmt_capacidad(s, c) != 0;
            if (!tx->activo) {
                s->registros[0xa0 / 4] |= 1u << (3 * c + 2);
            }
            return 1u << c;
        }
    } else if ((direccion >= 0x20 && direccion < 0x60) ||
               direccion == 0xa8 || (direccion >= 0xb0 && direccion <= 0xf0)) {
        if (direccion >= 0xd0 && direccion < 0xf0) {
            valor &= 511;
        }
        s->registros[direccion / 4] = valor;
    }
    return 0;
}

/* Leer solo al empezar este símbolo: la ISR puede haber rellenado esa RAM. */
static inline TallerRmtPaso taller_rmt_cargar(TallerRmt *s, unsigned canal)
{
    TallerRmtTx *tx = &s->tx[canal];
    TallerRmtPaso p = {0};
    if (!tx->activo) {
        return p;
    }
    unsigned capacidad = taller_rmt_capacidad(s, canal);
    p.valido = true;
    if (tx->posicion >= capacidad) {
        if ((s->registros[0xf0 / 4] & 2) && capacidad) {
            tx->posicion = 0;
        } else {
            p.fin = true;
            return p;
        }
    }
    p.simbolo = s->memoria[canal * 64 + tx->posicion];
    unsigned d0 = p.simbolo & 0x7fff;
    unsigned d1 = (p.simbolo >> 16) & 0x7fff;
    p.ticks = d0 ? d0 + d1 : 0;
    p.fin = !d0 || !d1;
    tx->tiempo_ps += (uint64_t)p.ticks * tx->tick_ps;
    return p;
}

/* Llamar tras consumir el tiempo del símbolo, nunca al cargarlo. */
static inline unsigned taller_rmt_avanzar(TallerRmt *s, unsigned canal,
                                          TallerRmtPaso p)
{
    TallerRmtTx *tx = &s->tx[canal];
    unsigned evento = 0;
    if (!p.valido || !tx->activo) {
        return 0;
    }
    if (p.ticks) {
        tx->posicion++;
        unsigned limite = s->registros[(0xd0 + canal * 4) / 4];
        if (limite && ++tx->desde_umbral >= limite) {
            tx->desde_umbral = 0;
            s->registros[0xa0 / 4] |= 1u << (24 + canal);
            evento |= TALLER_RMT_UMBRAL;
        }
    }
    bool borde = tx->posicion >= taller_rmt_capacidad(s, canal);
    if (p.fin || (borde && !(s->registros[0xf0 / 4] & 2))) {
        if ((s->registros[(0x24 + canal * 8) / 4] & 64) && tx->tiempo_ps) {
            tx->posicion = tx->desde_umbral = 0;
            return evento | TALLER_RMT_VUELTA;
        }
        tx->activo = false;
        s->registros[0xa0 / 4] |= 1u << (3 * canal);
        evento |= TALLER_RMT_FIN;
    } else if (borde) {
        tx->posicion = 0;
    }
    return evento;
}
