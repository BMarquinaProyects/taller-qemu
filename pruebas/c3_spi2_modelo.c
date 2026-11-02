/* SPI2 real aislado: se sustituyen servicios QEMU, no lógica de registros/DMA. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define OBJECT_CHECK(tipo,obj,nombre) ((tipo *)(obj))
#define ESP_GDMA_IN_IDX 0
#define ESP_GDMA_OUT_IDX 1
#define GDMA_SPI2 0
#define ESP_GDMA_RAM_ADDR 0x3fc80000u
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef uint64_t hwaddr;
typedef int *qemu_irq;
typedef struct SSIBus { int inutil; } SSIBus;
typedef struct { uint32_t raw, st, ena; qemu_irq irq; } DmaIntState;
typedef struct {
    uint32_t conf0, conf1, status, push_pop, link, state, suc_eof_desc_addr;
    uint32_t err_eof_desc_addr, desc_addr, bfr_desc_addr, bfr_bfr_desc_addr;
    uint32_t priority, peripheral;
    DmaIntState int_state;
} DmaConfigState;
typedef struct { DmaConfigState *ch_conf[2]; int dma_as; } ESPGdmaState;
static uint8_t ram[0x60000], mosi_emitido[100000];
static unsigned cantidad_emitida, tramas, ultima_trama;
static int nivel_irq, irq_dma[2][3];
static void qemu_set_irq(qemu_irq irq, int nivel) { *irq = nivel; }
static int address_space_read(int *as, uint32_t addr, int attrs, void *buf, size_t n)
{
    /* Como la dma_as real: direcciones absolutas de la SRAM del C3. */
    assert(addr >= ESP_GDMA_RAM_ADDR);
    addr -= ESP_GDMA_RAM_ADDR;
    assert(addr < sizeof(ram) && n <= sizeof(ram) - addr);
    memcpy(buf, ram + addr, n);
    return MEMTX_OK;
}
static int address_space_write(int *as, uint32_t addr, int attrs, const void *buf, size_t n)
{
    /* Como la dma_as real: direcciones absolutas de la SRAM del C3. */
    assert(addr >= ESP_GDMA_RAM_ADDR);
    addr -= ESP_GDMA_RAM_ADDR;
    assert(addr < sizeof(ram) && n <= sizeof(ram) - addr);
    memcpy(ram + addr, buf, n);
    return MEMTX_OK;
}
static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void stl_le_p(void *p, uint32_t v)
{
    uint8_t *b = p;
    for (unsigned i = 0; i < 4; i++) { b[i] = (uint8_t)(v >> (i * 8)); }
}
static void *g_malloc0(size_t n) { void *p = calloc(1, n); assert(p); return p; }
#define g_free free
static uint32_t ssi_transfer(SSIBus *bus, uint32_t v)
{
    assert(cantidad_emitida < sizeof(mosi_emitido));
    mosi_emitido[cantidad_emitida++] = (uint8_t)v;
    return 0xff;
}
static void taller_puente_spi_fin(uint8_t bus)
{
    assert(bus == 2);
    ultima_trama = cantidad_emitida;
    tramas++;
}
/* CABECERA_REAL */
/* MODELO_REAL */
static ESP32C3Spi2State spi;
static ESPGdmaState gdma;
static DmaConfigState canales[2][3];
static void escribir(unsigned a, uint32_t v) { esp32c3_spi2_write(&spi, a, v, 4); }
static uint32_t leer(unsigned a) { return (uint32_t)esp32c3_spi2_read(&spi, a, 4); }
static void reset(void)
{
    esp32c3_spi2_reiniciar(&spi, 0);
    memset(canales, 0, sizeof(canales));
    for (unsigned d = 0; d < 2; d++) {
        for (unsigned c = 0; c < 3; c++) {
            canales[d][c].int_state.irq = &irq_dma[d][c];
        }
    }
    memset(irq_dma, 0, sizeof(irq_dma));
    cantidad_emitida = tramas = 0;
    assert(!nivel_irq);
}
static void desc(unsigned dir, unsigned n, unsigned len, unsigned next, bool eof)
{
    unsigned a = 0x100 + dir * 0x400 + n * 12;
    stl_le_p(ram + a, 0x80000000u | (eof ? 0x40000000u : 0) |
              (dir ? len << 12 : 0) | len);
    stl_le_p(ram + a + 4, ESP_GDMA_RAM_ADDR + 0x1000 + dir * 0x10000 + n * 4092);
    stl_le_p(ram + a + 8, next ? ESP_GDMA_RAM_ADDR + a + 12 : 0);
}
static void arrancar(unsigned dir, unsigned canal)
{
    DmaConfigState *c = &canales[dir][canal];
    c->peripheral = 0;
    c->conf1 = 1u << 12; /* CHECK_OWNER */
    c->conf0 = 4; /* AUTO_WRBACK en OUT */
    c->link = ((ESP_GDMA_RAM_ADDR + 0x100 + dir * 0x400) & 0xfffff) |
              (1u << (dir ? 21 : 22));
    c->int_state.ena = 0x1f;
}
static void dma_larga(unsigned canal)
{
    reset();
    unsigned largo = 32768;
    for (unsigned i = 0; i < largo; i++) { ram[0x11000 + i] = (uint8_t)i; }
    for (unsigned d = 0; d < 2; d++) {
        unsigned pendiente = largo;
        for (unsigned i = 0; pendiente; i++) {
            unsigned n = MIN(4092, pendiente);
            desc(d, i, n, pendiente > n, pendiente == n);
            pendiente -= n;
        }
        arrancar(d, canal);
    }
    /* Canales anteriores parados o destinados a otro periférico no se tocan. */
    if (canal) { canales[1][0].peripheral = 7; canales[1][0].link |= 1u << 21; }
    escribir(0x10, (1u << 27) | (1u << 28) | 1);
    escribir(0x1c, largo * 8 - 1);
    escribir(0x30, (1u << 28) | (1u << 27));
    escribir(0x34, 1u << 12);
    escribir(0, 1u << 23); /* UPDATE no transmite ni completa. */
    assert(!leer(0) && !cantidad_emitida && !leer(0x3c));
    escribir(0, 1u << 24);
    assert(!leer(0) && cantidad_emitida == largo && tramas == 1 && ultima_trama == largo);
    for (unsigned i = 0; i < largo; i++) {
        assert(mosi_emitido[i] == (uint8_t)i && ram[0x1000 + i] == 0xff);
    }
    assert(nivel_irq && leer(0x40) == (1u << 12));
    assert(canales[1][canal].int_state.raw == 0xb);
    assert(canales[0][canal].int_state.raw == 3);
    assert(irq_dma[0][canal] && irq_dma[1][canal]);
    assert(!(canales[1][canal].link & (1u << 21)));
    assert(!(canales[0][canal].link & (1u << 22)));
    assert(!(ldl_le_p(ram + 0x500) & 0x80000000u));
    assert(!(ldl_le_p(ram + 0x100) & 0x80000000u));
    unsigned ultimo = 0x100 + 8 * 12;
    assert((ldl_le_p(ram + ultimo) & 0x40fff000u) == (0x40000000u | (32u << 12)));
    assert(canales[0][canal].suc_eof_desc_addr == ESP_GDMA_RAM_ADDR + ultimo);
    escribir(0x38, 1u << 12);
    assert(!nivel_irq && !leer(0x3c) && !leer(0x40) && !leer(0x38));
    /* Otro bloque completa el patrón de 40960 bytes, con START y cadena nuevos. */
    for (unsigned d = 0; d < 2; d++) {
        desc(d, 0, 4092, 1, false); desc(d, 1, 4092, 1, false);
        desc(d, 2, 8, 0, true); arrancar(d, canal);
    }
    escribir(0x1c, 8192 * 8 - 1); escribir(0, 1u << 24);
    assert(cantidad_emitida == 40960 && tramas == 2);
    for (unsigned i = 32768; i < 40960; i++) { assert(mosi_emitido[i] == (uint8_t)i); }
    /* Sin START nuevo: no volver a enviar datos del enlace ya consumido. */
    escribir(0, 1u << 24);
    assert(cantidad_emitida == 40960);
}

static void descriptores_invalidos(void)
{
    /* Ni descriptores ni buffers pueden salir de SRAM, incluso en el borde. */
    const uint32_t direcciones[] = {
        0x3fc7fffcu, 0x3fcdfff8u, 0x3fce0000u, 0x3fc80501u,
    };
    for (unsigned d = 0; d < 2; d++) {
        uint8_t datos[8] = {0};
        uint32_t error = d ? 4 : 8;
        for (unsigned i = 0; i < sizeof(direcciones) / sizeof(*direcciones); i++) {
            reset(); arrancar(d, 2);
            canales[d][2].link = (canales[d][2].link & ~0xfffffu) |
                                  (direcciones[i] & 0xfffff);
            assert(!esp32c3_spi2_dma_copiar(&spi, !d, datos, sizeof(datos)));
            assert(canales[d][2].int_state.raw == error && irq_dma[d][2]);
        }
        for (unsigned i = 0; i < 4; i++) {
            reset(); desc(d, 0, 8, 0, true); arrancar(d, 1);
            unsigned a = 0x100 + d * 0x400;
            stl_le_p(ram + a + 4, (uint32_t[]){
                0x60024000u, 0x3fc7fffcu, 0x3fcdfffcu, 0xfffffffcu,
            }[i]);
            assert(!esp32c3_spi2_dma_copiar(&spi, !d, datos, sizeof(datos)));
            assert(canales[d][1].int_state.raw == error);
            assert(ldl_le_p(ram + a) & C3_DMA_OWNER);
        }
        /* CHECK_OWNER rechaza CPU; sin CHECK_OWNER se permite consumirlo. */
        reset(); desc(d, 0, 8, 0, true); arrancar(d, 0);
        unsigned a = 0x100 + d * 0x400;
        stl_le_p(ram + a, ldl_le_p(ram + a) & ~C3_DMA_OWNER);
        assert(!esp32c3_spi2_dma_copiar(&spi, !d, datos, sizeof(datos)));
        assert(canales[d][0].int_state.raw == error);
        arrancar(d, 0); canales[d][0].conf1 = 0;
        assert(esp32c3_spi2_dma_copiar(&spi, !d, datos, sizeof(datos)) == sizeof(datos));
    }
    /* Un canal ajeno, parado o en reset nunca presta su descriptor a SPI2. */
    for (unsigned i = 0; i < 3; i++) {
        uint8_t datos[4];
        reset(); desc(1, 0, 4, 0, true); arrancar(1, 2);
        if (i == 0) { canales[1][2].peripheral = 7; }
        if (i == 1) { canales[1][2].link |= 1u << 20; }
        if (i == 2) { canales[1][2].conf0 |= 1; }
        assert(!esp32c3_spi2_dma_copiar(&spi, false, datos, sizeof(datos)));
        assert(!canales[1][2].int_state.raw);
        assert(ldl_le_p(ram + 0x500) & C3_DMA_OWNER);
    }
}

static void descriptores_parciales(void)
{
    uint8_t datos[4] = {0x12, 0x34, 0x56, 0x78};

    reset(); desc(0, 0, 16, 1, false); arrancar(0, 1);
    memset(ram + 0x1000, 0xa5, 20);
    uint32_t siguiente = ldl_le_p(ram + 0x108);
    assert(esp32c3_spi2_dma_copiar(&spi, true, datos, sizeof(datos)) == 4);
    assert(!memcmp(ram + 0x1000, datos, sizeof(datos)));
    assert(ram[0x1004] == 0xa5 && ram[0x1010] == 0xa5);
    assert(ldl_le_p(ram + 0x100) == (C3_DMA_EOF | (4u << 12) | 16));
    assert(ldl_le_p(ram + 0x108) == siguiente);
    assert(canales[0][1].int_state.raw == 3);

    /* MS_DLEN corta TX antes del fin del descriptor: no DONE/EOF ni writeback. */
    reset(); desc(1, 0, 16, 0, true); arrancar(1, 2);
    uint32_t ctrl = ldl_le_p(ram + 0x500);
    memcpy(ram + 0x11000, datos, sizeof(datos));
    escribir(0x10, C3_SPI_TX); escribir(0x1c, 31);
    escribir(0x30, C3_SPI_DMA_TX); escribir(0, C3_SPI_USR);
    assert(cantidad_emitida == 4 && !memcmp(mosi_emitido, datos, sizeof(datos)));
    assert(leer(0x3c) == C3_SPI_DONE);
    assert(ldl_le_p(ram + 0x500) == ctrl);
    assert(!canales[1][2].int_state.raw && !irq_dma[1][2]);
    /* OUT_AUTO_WRBACK apagado conserva OWNER incluso con descriptor completo. */
    reset(); desc(1, 0, 4, 0, true); arrancar(1, 0);
    canales[1][0].conf0 = 0;
    assert(esp32c3_spi2_dma_copiar(&spi, false, datos, sizeof(datos)) == 4);
    assert(ldl_le_p(ram + 0x500) & C3_DMA_OWNER);
    assert(canales[1][0].int_state.raw == 0xb);
}

static void fin_segmento(void)
{
    reset();
    escribir(0x34, C3_SPI_SEG_DONE);
    escribir(0xe0, 1u << 28); /* USR_CONF: cierre de segmento simplificado. */
    escribir(0x10, C3_SPI_TX); escribir(0x1c, 7); escribir(0x98, 0xa5);
    escribir(0, C3_SPI_USR);
    assert(cantidad_emitida == 1 && mosi_emitido[0] == 0xa5);
    assert(leer(0x3c) == C3_SPI_SEG_DONE && nivel_irq);
    /* Un fin normal posterior no borra el segmento pendiente sin CLR. */
    escribir(0xe0, 0); escribir(0, C3_SPI_USR);
    assert(leer(0x3c) == (C3_SPI_DONE | C3_SPI_SEG_DONE) && nivel_irq);
    escribir(0x38, C3_SPI_DONE); assert(nivel_irq);
    escribir(0x38, C3_SPI_SEG_DONE); assert(!nivel_irq);
}

int main(void)
{
    spi.irq = &nivel_irq;
    spi.gdma = &gdma;
    gdma.ch_conf[0] = canales[0]; gdma.ch_conf[1] = canales[1];
    descriptores_invalidos();
    descriptores_parciales();
    fin_segmento();
    reset();
    /* RAW R/W para que el driver despierte su ISR antes de primera transferencia. */
    escribir(0x3c, 1u << 12);
    assert(!nivel_irq && leer(0x3c) == (1u << 12));
    escribir(0x34, 1u << 12);
    assert(nivel_irq && leer(0x40) == (1u << 12));
    escribir(0x3c, 0); assert(!nivel_irq);
    escribir(0x10, (1u << 27) | (1u << 28) | 1);
    escribir(0x1c, 31); escribir(0x98, 0x64636261);
    escribir(0, 1u << 24);
    assert(cantidad_emitida == 4 && !memcmp(mosi_emitido, "abcd", 4) && tramas == 1);
    assert(leer(0x98) == 0xffffffffu && nivel_irq);
    /* Arduino no habilita USR_COMMAND: no aparece byte cero espurio. */
    escribir(0x18, 7u << 28); escribir(0x98, 0x1234); escribir(0x1c, 15);
    escribir(0, 1u << 24); assert(cantidad_emitida == 6 && mosi_emitido[4] == 0x34 && mosi_emitido[5] == 0x12);
    for (unsigned c = 0; c < 3; c++) { dma_larga(c); }
    reset(); escribir(0x10, 1u << 27); escribir(0x1c, 511);
    for (unsigned i = 0; i < 16; i++) {
        unsigned b = i * 4;
        escribir(0x98 + i * 4, b | (b + 1) << 8 | (b + 2) << 16 | (b + 3) << 24);
    }
    escribir(0, 1u << 24);
    assert(cantidad_emitida == 64);
    for (unsigned i = 0; i < 64; i++) { assert(mosi_emitido[i] == i); }
    reset();
    /* Dirección y comando activados: orden HAL; sin bytes de relleno implícitos. */
    escribir(0x10, (1u << 31) | (1u << 30) | (1u << 27));
    escribir(0x18, (15u << 28) | 0x3412); escribir(4, 0x56789a00);
    escribir(0x14, 23u << 27); escribir(0x98, 0xbc); escribir(0x1c, 7);
    escribir(0, 1u << 24);
    assert(cantidad_emitida == 6 && !memcmp(mosi_emitido, "\x12\x34\x56\x78\x9a\xbc", 6));
    /* RX sin TX: cuatro clocks a cero y MISO FF. */
    reset(); desc(0, 0, 4, 0, false); arrancar(0, 2);
    /* spi_master activa MOSI también en RX full duplex sin outlink. */
    escribir(0x98, 0xdeadbeef);
    escribir(0x10, (1u << 28) | (1u << 27) | 1);
    escribir(0x1c, 31); escribir(0x30, 1u << 27);
    escribir(0, 1u << 24);
    assert(cantidad_emitida == 4 && !memcmp(mosi_emitido, "\0\0\0\0", 4));
    assert(ldl_le_p(ram + 0x1000) == 0xffffffffu);
    /* DMA_TX_ENA puede seguir activo desde la operación anterior, sin START. */
    cantidad_emitida = 0; desc(0, 0, 4, 0, false); arrancar(0, 2);
    escribir(0x30, (1u << 27) | (1u << 28));
    escribir(0x38, 0x1fffff); escribir(0, 1u << 24);
    assert(cantidad_emitida == 4 && !memcmp(mosi_emitido, "\0\0\0\0", 4));
    assert(leer(0x3c) == (1u << 12));
    /* Descriptor inválido: error GDMA, fin SPI, sin emitir W0 como sustituto. */
    reset(); arrancar(1, 1); stl_le_p(ram + 0x500, 0);
    escribir(0x10, 1u << 27); escribir(0x1c, 511); escribir(0x30, 1u << 28);
    escribir(0, 1u << 24);
    assert(!cantidad_emitida && (canales[1][1].int_state.raw & 4));
    assert(leer(0x3c) & (1u << 12));
    /* Ciclo con CHECK_OWNER desactivado: no repetir el descriptor. */
    reset(); desc(1, 0, 4, 0, false); arrancar(1, 0);
    canales[1][0].conf1 = 0; canales[1][0].conf0 = 0;
    stl_le_p(ram + 0x508, ESP_GDMA_RAM_ADDR + 0x500);
    escribir(0x10, 1u << 27); escribir(0x1c, 63); escribir(0x30, 1u << 28);
    escribir(0, 1u << 24);
    assert(cantidad_emitida == 4 && (canales[1][0].int_state.raw & 4));
    /* CPU con longitud desmedida se acota; highpart empieza en W8. */
    reset(); escribir(0x10, (1u << 27) | (1u << 25));
    escribir(0xb8, 0xa5); escribir(0x1c, 0x3ffff); escribir(0, 1u << 24);
    assert(cantidad_emitida == 32 && mosi_emitido[0] == 0xa5);
    escribir(0x40, 0xffffffff); /* ST solo lectura */
    /* Fin de segmento: IRQ independiente de trans_done y CLR selectivo. */
    escribir(0x38, 0x1fffff); escribir(0x34, 1u << 13);
    escribir(0x3c, 1u << 13); assert(nivel_irq);
    escribir(0x38, 1u << 12); assert(nivel_irq);
    escribir(0x38, 1u << 13); assert(!nivel_irq);
    escribir(0xe0, 1u << 27); assert(!(leer(0xe0) & (1u << 27)));
    esp32c3_spi2_reiniciar(&spi, 0); assert(!nivel_irq && !leer(0x3c));
    assert(!leer(0x1000) && !leer(0x99));
    puts("OK SPI2 C3 real: CPU, GDMA 0/1/2, 40960 bytes, SRAM/OWNER, IRQ y segmento");
    return 0;
}
