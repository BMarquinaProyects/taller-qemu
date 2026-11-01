/* SPI DMA del ESP32 clásico. Copyright (c) 2026 Taller.
 * Licencia: GPL-2.0-or-later. Lógica independiente de QEMU. */
#ifndef TALLER_SPI_DMA_H
#define TALLER_SPI_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TALLER_SPI_DMA_START (UINT32_C(1) << 29)
#define TALLER_SPI_DMA_STOP (UINT32_C(1) << 28)
#define TALLER_SPI_DMA_RESTART (UINT32_C(1) << 30)
#define TALLER_SPI_DMA_OWNER (UINT32_C(1) << 31)
#define TALLER_SPI_DMA_EOF (UINT32_C(1) << 30)
#define TALLER_SPI_DMA_IRQ_MASK UINT32_C(0x1ff)
#define TALLER_SPI_DMA_IN_EMPTY UINT32_C(1)
#define TALLER_SPI_DMA_OUT_ERROR UINT32_C(2)
#define TALLER_SPI_DMA_IN_ERROR UINT32_C(4)
#define TALLER_SPI_DMA_IN_DONE UINT32_C(8)
#define TALLER_SPI_DMA_IN_EOF UINT32_C(32)
#define TALLER_SPI_DMA_OUT_DONE UINT32_C(64)
#define TALLER_SPI_DMA_OUT_EOF UINT32_C(128)
#define TALLER_SPI_DMA_OUT_TOTAL_EOF UINT32_C(256)

typedef struct TallerSpiDma {
    uint32_t conf, out_link, in_link, raw, ena;
    uint32_t debug[12]; /* 0x120..0x14c, registros de descriptores/estado. */
    bool out_active, in_active;
} TallerSpiDma;

typedef struct TallerSpiDmaMemoria {
    void *opaque;
    bool (*leer)(void *, uint32_t, void *, size_t);
    bool (*escribir)(void *, uint32_t, const void *, size_t);
} TallerSpiDmaMemoria;

static inline unsigned taller_spi_dma_canal(uint32_t seleccion, unsigned spi)
{
    unsigned canal;
    if (spi < 2 || spi > 3) {
        return 0;
    }
    canal = (seleccion >> ((spi - 1) * 2)) & 3;
    return canal < 3 ? canal : 0;
}

static inline uint32_t taller_spi_dma_leer(const TallerSpiDma *s, unsigned reg)
{
    switch (reg) {
    case 0x100: return s->conf;
    case 0x104: return s->out_link;
    case 0x108: return s->in_link;
    case 0x10c: return (s->in_active ? 1 : 0) | (s->out_active ? 2 : 0);
    case 0x110: return s->ena;
    case 0x114: return s->raw;
    case 0x118: return s->raw & s->ena;
    default:
        if (reg >= 0x120 && reg <= 0x14c && !(reg & 3)) {
            return s->debug[(reg - 0x120) / 4];
        }
        return 0;
    }
}

static inline void taller_spi_dma_escribir(TallerSpiDma *s, unsigned reg, uint32_t v)
{
    switch (reg) {
    case 0x100:
        s->conf = v & 0x1dffc;
        /* Los resets FIFO/AHB (0x30) no desmontan enlaces: IDF los pulsa
         * al preparar TX DESPUÉS de arrancar RX. Solo in_rst/out_rst paran
         * sus respectivos motores. No hay FIFO pendiente en este modelo. */
        if (v & 0x04) {
            s->in_active = false;
            memset(s->debug, 0, 5 * sizeof(uint32_t));
        }
        if (v & 0x08) {
            s->out_active = false;
            memset(s->debug + 5, 0, 5 * sizeof(uint32_t));
        }
        break;
    case 0x104:
    case 0x108: {
        bool rx = reg == 0x108;
        bool *activo = rx ? &s->in_active : &s->out_active;
        uint32_t *link = rx ? &s->in_link : &s->out_link;
        *link = v & (rx ? 0x1fffff : 0xfffff);
        if (v & TALLER_SPI_DMA_STOP) {
            *activo = false;
        } else if (v & (TALLER_SPI_DMA_START | TALLER_SPI_DMA_RESTART)) {
            *activo = true;
        }
        break;
    }
    case 0x110: s->ena = v & TALLER_SPI_DMA_IRQ_MASK; break;
    case 0x11c: s->raw &= ~(v & TALLER_SPI_DMA_IRQ_MASK); break;
    default: break;
    }
}

static inline uint32_t taller_spi_dma_u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline bool taller_spi_dma_sram(uint32_t dir, size_t n)
{
    /* Solo SRAM interna. Nunca ejecutar MMIO por un descriptor mal formado. */
    return dir >= UINT32_C(0x3ffae000) && dir < UINT32_C(0x40000000) &&
           n <= UINT32_C(0x40000000) - dir;
}

/* Copia entre la cadena guest y un bloque de la transacción SPI. TX usa length;
 * RX usa size y escribe length/eof. offset/sosf son reservados en SPI (TRM 2.3.2),
 * se conservan sin interpretarlos. El límite es MOSI/MISO_DLEN, no W0..W15.
 * Los eventos se acumulan en RAW; QEMU actualiza las IRQ al terminar CMD.usr.
 */
static inline size_t taller_spi_dma_copiar(TallerSpiDma *s,
    const TallerSpiDmaMemoria *mem, bool rx, uint8_t *datos, size_t cantidad)
{
    bool *activo = rx ? &s->in_active : &s->out_active;
    uint32_t dir = UINT32_C(0x3ff00000) |
                   ((rx ? s->in_link : s->out_link) & 0xfffff);
    uint32_t error = rx ? TALLER_SPI_DMA_IN_ERROR : TALLER_SPI_DMA_OUT_ERROR;
    uint32_t eventos = 0;
    unsigned dbg = rx ? 2 : 7;
    unsigned nodos;
    size_t total = 0;

    if (!*activo || !cantidad) {
        return 0;
    }
    /* En SRAM caben menos de 32768 descriptores distintos de 12 bytes.
     * Además, cada descriptor se devuelve a CPU: un ciclo falla por owner.
     */
    for (nodos = 0; total < cantidad && nodos < 32768; nodos++) {
        uint8_t d[12];
        uint32_t ctrl, buf, sig, capacidad, largo;
        size_t n;
        bool eof;

        s->debug[dbg] = dir;
        if ((dir & 3) || !taller_spi_dma_sram(dir, sizeof(d)) ||
            !mem->leer(mem->opaque, dir, d, sizeof(d))) {
            eventos |= error;
            break;
        }
        ctrl = taller_spi_dma_u32(d);
        buf = taller_spi_dma_u32(d + 4);
        sig = taller_spi_dma_u32(d + 8);
        capacidad = ctrl & 0xfff;
        largo = (ctrl >> 12) & 0xfff;
        s->debug[dbg + 1] = sig;
        s->debug[dbg + 2] = buf;
        if (!(ctrl & TALLER_SPI_DMA_OWNER) || (!rx && largo > capacidad) ||
            (rx && !capacidad)) {
            eventos |= error;
            break;
        }
        n = rx ? capacidad : largo;
        if (n > cantidad - total) {
            n = cantidad - total;
        }
        if (!taller_spi_dma_sram(buf, n) ||
            !(rx ? mem->escribir(mem->opaque, buf, datos + total, n) :
                   mem->leer(mem->opaque, buf, datos + total, n))) {
            eventos |= error;
            break;
        }
        eof = rx ? total + n == cantidad :
                    (ctrl & TALLER_SPI_DMA_EOF) && n == largo;
        ctrl &= ~TALLER_SPI_DMA_OWNER;
        if (rx) {
            ctrl = (ctrl & ~(UINT32_C(0xfff000) | TALLER_SPI_DMA_EOF)) |
                   (uint32_t)n << 12 | (eof ? TALLER_SPI_DMA_EOF : 0);
        }
        for (unsigned i = 0; i < 4; i++) { d[i] = ctrl >> (8 * i); }
        if (!mem->escribir(mem->opaque, dir, d, 4)) {
            eventos |= error;
            break;
        }
        total += n;
        eventos |= rx ? TALLER_SPI_DMA_IN_DONE : TALLER_SPI_DMA_OUT_DONE;
        if (eof) {
            eventos |= rx ? TALLER_SPI_DMA_IN_EOF :
                         TALLER_SPI_DMA_OUT_EOF | TALLER_SPI_DMA_OUT_TOTAL_EOF;
            if (rx) {
                s->debug[1] = dir;
            } else {
                s->debug[5] = buf;
                s->debug[6] = dir;
            }
            break;
        }
        if (total == cantidad) {
            break;
        }
        if (!sig) {
            eventos |= rx ? TALLER_SPI_DMA_IN_EMPTY : error;
            break;
        }
        dir = sig;
    }
    if (nodos == 32768 && total < cantidad) { eventos |= error; }
    if (rx && (eventos & TALLER_SPI_DMA_IN_ERROR)) { s->debug[0] = dir; }
    s->raw |= eventos;
    *activo = false;
    return total;
}
#endif
