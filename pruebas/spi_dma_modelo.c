/* Banco del modelo SPI real. Solo se sustituyen servicios de QEMU y SSI. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hw/ssi/taller_spi_dma.h"

typedef uint64_t hwaddr;
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int *qemu_irq;
typedef struct SSIBus { int children; } SSIBus;
typedef int Esp32FlashEncryptionState;
#define OBJECT_CHECK(type, obj, name) ((type *)(obj))
#define REG32(reg, addr) enum { A_ ## reg = addr };
#define FIELD(reg, field, shift, len) enum { R_ ## reg ## _ ## field ## _SHIFT = shift, \
    R_ ## reg ## _ ## field ## _MASK = ((UINT32_C(1) << (len)) - 1) << (shift) };
#define FIELD_EX32(v, reg, field) (((v) & R_ ## reg ## _ ## field ## _MASK) >> R_ ## reg ## _ ## field ## _SHIFT)
#define FIELD_DP32(v, reg, field, n) (((v) & ~R_ ## reg ## _ ## field ## _MASK) | ((n) << R_ ## reg ## _ ## field ## _SHIFT))
#define BIT(n) (UINT32_C(1) << (n))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BUS(s) (s)
#define QTAILQ_EMPTY(p) (*(p) == 0)
#define bswap32(n) __builtin_bswap32(n)
#define g_malloc0(n) calloc(1, (n))
static void liberar(void *p) { free(*(void **)p); }
#define g_autofree __attribute__((cleanup(liberar)))
#define MEMTX_OK 0
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static uint8_t ram[0x52000], cable[100000];
static size_t ncable, finales;
static int irq, dma_irq;
static bool respuesta_variable;

static void qemu_set_irq(qemu_irq q, int nivel) { *q = nivel; }
static uint32_t ssi_transfer(SSIBus *bus, uint32_t v)
{
    assert(ncable < sizeof(cable));
    cable[ncable++] = v;
    return bus->children ? (respuesta_variable ? v ^ 0xa5 : 0xff) : 0;
}
static void taller_puente_spi_fin(uint8_t bus) { (void)bus; finales++; }
static int address_space_read(int *as, uint32_t dir, int attrs, void *p, size_t n)
{
    (void)as; (void)attrs;
    if (!taller_spi_dma_sram(dir, n)) { return 1; }
    memcpy(p, ram + dir - 0x3ffae000, n);
    return 0;
}
static int address_space_write(int *as, uint32_t dir, int attrs, const void *p, size_t n)
{
    (void)as; (void)attrs;
    if (!taller_spi_dma_sram(dir, n)) { return 1; }
    memcpy(ram + dir - 0x3ffae000, p, n);
    return 0;
}
static Esp32FlashEncryptionState *esp32_flash_encryption_find(void) { return NULL; }
static bool esp32_flash_encryption_enabled(Esp32FlashEncryptionState *s) { (void)s; return false; }
static void esp32_flash_encryption_get_result(Esp32FlashEncryptionState *s, uint32_t *p, int n)
{ (void)s; (void)p; (void)n; }

/* CABECERA_REAL */
/* MODELO_REAL */

static void desc(unsigned pos, uint32_t ctrl, uint32_t buf, uint32_t sig)
{
    uint32_t palabras[] = {ctrl, buf, sig};
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned j = 0; j < 4; j++) { ram[pos + i * 4 + j] = palabras[i] >> (8 * j); }
    }
}

static void iniciar(Esp32SpiState *s, SSIBus *bus, uint32_t *seleccion, unsigned index)
{
    memset(s, 0, sizeof(*s));
    s->spi = bus; s->irq = &irq; s->dma_irq = &dma_irq;
    s->dma_chan_sel = seleccion; s->index = index;
    for (unsigned i = 0; i < 3; i++) { s->cs_gpio[i] = &irq; }
    esp32_spi_reset_hold(s, 0);
    ncable = finales = 0;
}

static void escribir(Esp32SpiState *s, unsigned reg, uint32_t v)
{ esp32_spi_write(s, reg, v, 4); }
static uint32_t leer(Esp32SpiState *s, unsigned reg)
{ return esp32_spi_read(s, reg, 4); }

int main(void)
{
    Esp32SpiState s;
    SSIBus bus = {1};
    uint32_t seleccion = 0x24;
    /* Bus HSPI y VSPI: dos descriptores, dúplex, datos reales de SSI y dos IRQ. */
    for (unsigned index = 2; index <= 3; index++) {
        iniciar(&s, &bus, &seleccion, index);
        desc(0, 0x80004004, 0x3ffae100, 0x3ffae00c);
        desc(12, 0xc0004004, 0x3ffae104, 0);
        desc(24, 0x8000000c, 0x3ffae120, 0);
        memcpy(ram + 256, "abcdefgh", 8);
        memset(ram + 288, 0x55, 12);
        escribir(&s, A_SPI_SLAVE, 0x210); /* done R/W despierta ISR inactiva */
        assert(irq == 1 && leer(&s, A_SPI_SLAVE) == 0x210);
        escribir(&s, A_SPI_SLAVE, 0x200);
        assert(irq == 0);
        escribir(&s, 0x110, 0x1ff);
        /* Secuencia real IDF: RX primero, después reset/FIFO/start de TX. */
        escribir(&s, 0x100, 0x04);
        escribir(&s, 0x100, 0);
        escribir(&s, 0x100, 0x30);
        escribir(&s, 0x100, 0);
        escribir(&s, 0x108, 0x200ae018);
        escribir(&s, 0x100, 0x08);
        escribir(&s, 0x100, 0);
        escribir(&s, 0x100, 0x30);
        escribir(&s, 0x100, 0);
        assert(leer(&s, 0x10c) == 1);
        escribir(&s, 0x104, 0x200ae000);
        escribir(&s, A_SPI_USER, 0x18000001);
        escribir(&s, A_SPI_MOSI_DLEN, 63);
        escribir(&s, A_SPI_MISO_DLEN, 63);
        respuesta_variable = true;
        escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
        assert(ncable == 8 && memcmp(cable, "abcdefgh", 8) == 0 && finales == 1);
        for (unsigned i = 0; i < 8; i++) { assert(ram[288 + i] == ((uint8_t)"abcdefgh"[i] ^ 0xa5)); }
        assert(ram[296] == 0x55 && ram[3] == 0 && ram[15] == 0x40 && ram[27] == 0x40);
        assert(leer(&s, 0x114) == 0x1e8 && leer(&s, 0x118) == 0x1e8);
        assert(irq == 1 && dma_irq == 1 && leer(&s, A_SPI_CMD) == 0);
        escribir(&s, A_SPI_SLAVE, 0x200);
        assert(irq == 0 && dma_irq == 1);
        escribir(&s, 0x11c, 0x1ff);
        assert(dma_irq == 0 && leer(&s, 0x118) == 0);
        escribir(&s, A_SPI_SLAVE, 0x210);
        escribir(&s, A_SPI_SLAVE, 0x10);
        assert(irq == 0); /* deshabilitar IRQ conserva RAW */
    }
    /* PIO Arduino: sin DMA/canal, W0..W15, sin comando espurio. */
    iniciar(&s, &bus, &seleccion, 3);
    escribir(&s, A_SPI_USER, 0x18000001);
    escribir(&s, A_SPI_W0, 0xff005aa5);
    escribir(&s, A_SPI_MOSI_DLEN, 31);
    escribir(&s, A_SPI_MISO_DLEN, 31);
    respuesta_variable = false;
    escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
    assert(ncable == 4 && memcmp(cable, "\xa5\x5a\0\xff", 4) == 0);
    assert(leer(&s, A_SPI_W0) == 0xffffffff);
    /* RX solo DMA, sin esclavo, MISO a FF y MOSI clocks a cero. */
    iniciar(&s, &bus, &seleccion, 3);
    bus.children = 0;
    desc(0, 0x80000008, 0x3ffae100, 0);
    escribir(&s, 0x108, 0x200ae000);
    escribir(&s, A_SPI_USER, 0x10000001);
    escribir(&s, A_SPI_MISO_DLEN, 31);
    escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
    assert(ncable == 4 && memcmp(cable, "\0\0\0\0", 4) == 0);
    assert(memcmp(ram + 256, "\xff\xff\xff\xff", 4) == 0);
    /* Workaround IDF: inlink NULL durante TX, sin error RX. */
    iniciar(&s, &bus, &seleccion, 3);
    desc(0, 0xc0004004, 0x3ffae100, 0);
    memcpy(ram + 256, "abcd", 4);
    escribir(&s, 0x108, 0x20000000);
    escribir(&s, 0x104, 0x200ae000);
    escribir(&s, A_SPI_USER, 0x08000001);
    escribir(&s, A_SPI_MOSI_DLEN, 31);
    escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
    assert(ncable == 4 && leer(&s, 0x114) == 0x1c0);
    /* STOP impide acceder al descriptor; DMA deseleccionado usa registros. */
    iniciar(&s, &bus, &seleccion, 3);
    escribir(&s, 0x104, 0x20000000);
    escribir(&s, 0x104, 0x10000000);
    escribir(&s, A_SPI_W0, 0x64636261);
    escribir(&s, A_SPI_USER, 0x08000001);
    escribir(&s, A_SPI_MOSI_DLEN, 31);
    escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
    assert(ncable == 4 && leer(&s, 0x114) == 0 && memcmp(cable, "abcd", 4) == 0);
    seleccion = 0;
    ncable = 0;
    escribir(&s, 0x104, 0x20000000);
    escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
    assert(ncable == 4 && leer(&s, 0x114) == 0);
    /* Una sola CMD.usr debe transmitir el framebuffer entero, no 64 bytes. */
    seleccion = 0x20;
    iniciar(&s, &bus, &seleccion, 3);
    for (unsigned i = 0, pos = 0; pos < 40960; i++) {
        unsigned n = MIN(4092U, 40960 - pos);
        bool fin = pos + n == 40960;
        desc(i * 12, 0x80000000 | (fin ? 0x40000000 : 0) | n << 12 | n,
             0x3ffaf000 + pos, fin ? 0 : 0x3ffae000 + (i + 1) * 12);
        for (unsigned j = 0; j < n; j++) { ram[4096 + pos + j] = (pos + j) & 255; }
        pos += n;
    }
    escribir(&s, 0x104, 0x200ae000);
    escribir(&s, A_SPI_USER, 0x08000001);
    escribir(&s, A_SPI_MOSI_DLEN, 40960 * 8 - 1);
    escribir(&s, A_SPI_CMD, R_SPI_CMD_USR_MASK);
    assert(ncable == 40960 && finales == 1 && leer(&s, 0x114) == 0x1c0);
    for (unsigned j = 0; j < 40960; j++) { assert(cable[j] == (j & 255)); }
    esp32_spi_reset_hold(&s, 0);
    assert(leer(&s, 0x10c) == 0 && leer(&s, 0x110) == 0 && irq == 0 && dma_irq == 0);
    puts("OK modelo SPI real: HSPI/VSPI, SSI, PIO, RX, IRQ, STOP, canal y reset");
    return 0;
}
