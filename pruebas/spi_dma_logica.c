/* Pruebas de contratos DMA: SRAM guest, sin cabeceras de QEMU. */
#include <assert.h>
#include <stdio.h>
#include "hw/ssi/taller_spi_dma.h"

#define BASE UINT32_C(0x3ffae000)
static uint8_t ram[0x52000];
static unsigned accesos;
static uint32_t fallo;

static bool leer(void *op, uint32_t dir, void *p, size_t n)
{
    (void)op;
    accesos++;
    if (dir == fallo || dir < BASE || n > sizeof(ram) || dir - BASE > sizeof(ram) - n) {
        return false;
    }
    memcpy(p, ram + dir - BASE, n);
    return true;
}

static bool escribir(void *op, uint32_t dir, const void *p, size_t n)
{
    (void)op;
    accesos++;
    if (dir == fallo || dir < BASE || n > sizeof(ram) || dir - BASE > sizeof(ram) - n) {
        return false;
    }
    memcpy(ram + dir - BASE, p, n);
    return true;
}

static void palabra(uint32_t dir, uint32_t v)
{
    uint8_t p[4] = {v, v >> 8, v >> 16, v >> 24};
    assert(escribir(NULL, dir, p, 4));
}

static uint32_t valor(uint32_t dir)
{
    uint8_t *p = ram + dir - BASE;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void descriptor(uint32_t dir, uint32_t ctrl, uint32_t buf, uint32_t sig)
{
    palabra(dir, ctrl);
    palabra(dir + 4, buf);
    palabra(dir + 8, sig);
}

static TallerSpiDmaMemoria memoria = {NULL, leer, escribir};

static void registros(void)
{
    TallerSpiDma s = {0};
    assert(taller_spi_dma_canal(0x24, 2) == 1);
    assert(taller_spi_dma_canal(0x24, 3) == 2);
    assert(taller_spi_dma_canal(0x3c, 2) == 0); /* 3 reservado */
    assert(taller_spi_dma_canal(0x3f, 0) == 0);
    taller_spi_dma_escribir(&s, 0x104, TALLER_SPI_DMA_START | (BASE & 0xfffff));
    assert(s.out_active && taller_spi_dma_leer(&s, 0x10c) == 2);
    assert(taller_spi_dma_leer(&s, 0x104) == (BASE & 0xfffff)); /* start autolimpiable */
    taller_spi_dma_escribir(&s, 0x104, TALLER_SPI_DMA_STOP | (BASE & 0xfffff));
    assert(!s.out_active);
    taller_spi_dma_escribir(&s, 0x104, TALLER_SPI_DMA_RESTART | (BASE & 0xfffff));
    assert(s.out_active);
    taller_spi_dma_escribir(&s, 0x108, TALLER_SPI_DMA_START | (BASE & 0xfffff));
    taller_spi_dma_escribir(&s, 0x100, 0x30); /* preparar TX tras arrancar RX */
    assert(s.in_active && s.out_active); /* FIFO/AHB no desmontan enlaces */
    taller_spi_dma_escribir(&s, 0x100, 0x1c04); /* in_rst, conserva configuración */
    assert(!s.in_active && s.out_active && s.conf == 0x1c04);
    taller_spi_dma_escribir(&s, 0x100, 0x1c00);
    taller_spi_dma_escribir(&s, 0x100, 0x1c08);
    assert(!s.out_active);
    s.raw = 0x1c8;
    taller_spi_dma_escribir(&s, 0x110, 0x1c0);
    assert(taller_spi_dma_leer(&s, 0x118) == 0x1c0);
    taller_spi_dma_escribir(&s, 0x11c, 0x180);
    assert(taller_spi_dma_leer(&s, 0x114) == 0x48);
    assert(taller_spi_dma_leer(&s, 0x118) == 0x40);
    taller_spi_dma_escribir(&s, 0x114, 0); /* RAW/ST/estados solo lectura */
    assert(s.raw == 0x48 && taller_spi_dma_leer(&s, 0x11c) == 0);
}

static void tx_largo(void)
{
    TallerSpiDma s = {0};
    uint8_t datos[40960];
    size_t pos = 0;
    memset(ram, 0xa5, sizeof(ram));
    for (unsigned i = 0; pos < sizeof(datos); i++) {
        size_t n = sizeof(datos) - pos;
        if (n > 4092) { n = 4092; }
        bool fin = pos + n == sizeof(datos);
        descriptor(BASE + i * 12, 0x80000000 | (fin ? 0x40000000 : 0) |
                   (uint32_t)n << 12 | (uint32_t)n, BASE + 4096 + pos,
                   fin ? 0 : BASE + (i + 1) * 12);
        for (size_t j = 0; j < n; j++) { ram[4096 + pos + j] = (pos + j) & 255; }
        pos += n;
    }
    taller_spi_dma_escribir(&s, 0x104, 0x20000000 | (BASE & 0xfffff));
    assert(taller_spi_dma_copiar(&s, &memoria, false, datos, sizeof(datos)) == sizeof(datos));
    for (size_t j = 0; j < sizeof(datos); j++) { assert(datos[j] == (j & 255)); }
    for (unsigned i = 0; i < 11; i++) { assert(!(valor(BASE + i * 12) & 0x80000000)); }
    assert(s.raw == 0x1c0 && !s.out_active);
    assert(taller_spi_dma_leer(&s, 0x138) == BASE + 120);
    assert(taller_spi_dma_leer(&s, 0x13c) == BASE + 120);
}

static void limites_y_errores(void)
{
    uint8_t datos[16] = {0};
    for (unsigned caso = 0; caso < 8; caso++) {
        TallerSpiDma s = {0};
        memset(ram, 0, sizeof(ram));
        fallo = 0;
        descriptor(BASE, 0xc0004004, BASE + 64, 0);
        memcpy(ram + 64, "abcdEFGH", 8);
        size_t n = 8, esperado = 4;
        uint32_t irq = 0x1c0;
        switch (caso) {
        case 0: break; /* eof temprano, no lee más */
        case 1: n = 2; esperado = 2; irq = 0x40; break; /* MOSI menor que length */
        case 2: palabra(BASE, 0x40004004); esperado = 0; irq = 2; break; /* owner CPU */
        case 3: palabra(BASE, 0xc0005004); esperado = 0; irq = 2; break; /* length > size */
        case 4: palabra(BASE, 0x80004004); irq = 0x42; break; /* falta siguiente */
        case 5: palabra(BASE, 0x80000004); palabra(BASE + 8, BASE); esperado = 0; irq = 0x42; break;
        case 6: palabra(BASE + 4, 0x60000000); esperado = 0; irq = 2; break; /* MMIO */
        case 7: fallo = BASE + 64; esperado = 0; irq = 2; break;
        }
        taller_spi_dma_escribir(&s, 0x104, 0x20000000 | (BASE & 0xfffff));
        accesos = 0;
        assert(taller_spi_dma_copiar(&s, &memoria, false, datos, n) == esperado);
        assert(s.raw == irq && accesos < 100 && !s.out_active);
        if (esperado) { assert(memcmp(datos, "abcd", esperado) == 0); }
    }
    fallo = 0;
}

static void rx(void)
{
    uint8_t datos[12];
    memset(datos, 0xff, sizeof(datos));
    memset(ram, 0xa5, sizeof(ram));
    TallerSpiDma s = {0};
    descriptor(BASE, 0x80000004, BASE + 64, BASE + 12);
    descriptor(BASE + 12, 0x80000008, BASE + 80, 0);
    taller_spi_dma_escribir(&s, 0x108, 0x20000000 | (BASE & 0xfffff));
    assert(taller_spi_dma_copiar(&s, &memoria, true, datos, 7) == 7);
    assert(valor(BASE) == 0x00004004);
    assert(valor(BASE + 12) == 0x40003008);
    assert(memcmp(ram + 64, datos, 4) == 0 && ram[68] == 0xa5);
    assert(memcmp(ram + 80, datos, 3) == 0 && ram[83] == 0xa5);
    assert(s.raw == 0x28 && taller_spi_dma_leer(&s, 0x124) == BASE + 12);
    s = (TallerSpiDma){0};
    descriptor(BASE, 0x80000004, BASE + 64, 0);
    taller_spi_dma_escribir(&s, 0x108, 0x20000000 | (BASE & 0xfffff));
    assert(taller_spi_dma_copiar(&s, &memoria, true, datos, 8) == 4);
    assert(s.raw == 9 && !(valor(BASE) & 0xc0000000));
    s = (TallerSpiDma){0};
    descriptor(BASE, 0x00000004, BASE + 64, 0);
    taller_spi_dma_escribir(&s, 0x108, 0x20000000 | (BASE & 0xfffff));
    assert(taller_spi_dma_copiar(&s, &memoria, true, datos, 4) == 0 && s.raw == 4);
    assert(taller_spi_dma_leer(&s, 0x120) == BASE);
    /* RAW queda retenido; una RX válida posterior no cambia dirección de error. */
    descriptor(BASE + 12, 0x80000004, BASE + 80, 0);
    taller_spi_dma_escribir(&s, 0x108, 0x20000000 | ((BASE + 12) & 0xfffff));
    assert(taller_spi_dma_copiar(&s, &memoria, true, datos, 4) == 4 && s.raw == 0x2c);
    assert(taller_spi_dma_leer(&s, 0x120) == BASE);
}

int main(void)
{
    registros(); tx_largo(); limites_y_errores(); rx();
    puts("OK lógica SPI DMA: registros, TX 40960, RX, límites y errores");
    return 0;
}
