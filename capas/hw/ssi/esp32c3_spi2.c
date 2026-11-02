/* SPI2 maestro C3: FIFO CPU y descriptores del GDMA compartido de Espressif.
 * Registros contrastados con spi_{reg,struct}.h y spi_ll.h de Arduino 3.3.12.
 * GPL-2.0-or-later. Operaciones síncronas, MISO en reposo a FF.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/irq.h"
#include "hw/ssi/esp32c3_spi2.h"
#include "hw/misc/taller_puente.h"

/* Estado y registros independientes del SPI1 de flash y del SPI clásico. */
#define C3_SPI_CMD 0x00
#define C3_SPI_USER 0x10
#define C3_SPI_DLEN 0x1c
#define C3_SPI_DMA 0x30
#define C3_SPI_ENA 0x34
#define C3_SPI_CLR 0x38
#define C3_SPI_RAW 0x3c
#define C3_SPI_ST 0x40
#define C3_SPI_W0 0x98
#define C3_SPI_SLAVE 0xe0
#define C3_SPI_USR (1u << 24)
#define C3_SPI_DONE (1u << 12)
#define C3_SPI_SEG_DONE (1u << 13)
#define C3_SPI_TX (1u << 27)
#define C3_SPI_RX (1u << 28)
#define C3_SPI_DMA_TX (1u << 28)
#define C3_SPI_DMA_RX (1u << 27)
#define C3_SPI_IRQ_MASK 0x1fffffu
#define C3_DMA_OWNER (1u << 31)
#define C3_DMA_EOF (1u << 30)
#define C3_DMA_SRAM_FIN 0x3fce0000u

static void esp32c3_spi2_irq(ESP32C3Spi2State *s)
{
    s->registros[C3_SPI_ST / 4] = s->registros[C3_SPI_RAW / 4] &
                                 s->registros[C3_SPI_ENA / 4];
    /* También recalcular al escribir RAW/ENA: IDF pone fin a uno para arrancar
     * su ISR. La matriz C3 conserva el nivel al reasignar la fuente (E-039). */
    qemu_set_irq(s->irq, s->registros[C3_SPI_ST / 4] != 0);
}

static void esp32c3_spi2_gdma_irq(DmaConfigState *c, uint32_t eventos)
{
    c->int_state.raw |= eventos;
    c->int_state.st = c->int_state.raw & c->int_state.ena;
    qemu_set_irq(c->int_state.irq, c->int_state.st != 0);
}

static DmaConfigState *esp32c3_spi2_canal(ESP32C3Spi2State *s, bool rx)
{
    unsigned dir = rx ? ESP_GDMA_IN_IDX : ESP_GDMA_OUT_IDX;
    unsigned inicio = rx ? 22 : 21;
    unsigned parada = rx ? 21 : 20;

    if (!s->gdma) {
        return NULL;
    }
    /* No usar get_channel_periph de la base: combina periférico y START con
     * OR y prueba el bit START de OUT incluso al buscar IN. Aquí se requieren
     * ambas condiciones y nunca se consume un canal ajeno o ya terminado. */
    for (unsigned n = 0; n < 3; n++) {
        DmaConfigState *c = &s->gdma->ch_conf[dir][n];

        if ((c->peripheral & 63) == GDMA_SPI2 &&
            (c->link & (1u << inicio)) && !(c->link & (1u << parada)) &&
            !(c->conf0 & 1)) {
            return c;
        }
    }
    return NULL;
}

static bool esp32c3_spi2_memoria(ESP32C3Spi2State *s, bool escribir,
                               uint32_t dir, void *datos, size_t n)
{
    /* dma_as tiene como raíz el alias DRAM, que está en 0x3FC80000 dentro de
     * sys_mem: QEMU conserva ese desplazamiento al aplanar la vista, así que las
     * direcciones son absolutas, como en esp_gdma.c. Solo SRAM C3: un
     * descriptor nunca ejecuta MMIO ni accede a la flash. */
    if (dir < ESP_GDMA_RAM_ADDR || dir >= C3_DMA_SRAM_FIN ||
        n > C3_DMA_SRAM_FIN - dir) {
        return false;
    }
    return (escribir ? address_space_write(&s->gdma->dma_as, dir,
                         MEMTXATTRS_UNSPECIFIED, datos, n) :
                       address_space_read(&s->gdma->dma_as, dir,
                         MEMTXATTRS_UNSPECIFIED, datos, n)) == MEMTX_OK;
}

static size_t esp32c3_spi2_dma_copiar(ESP32C3Spi2State *s, bool rx,
                                    uint8_t *datos, size_t cantidad)
{
    DmaConfigState *c = esp32c3_spi2_canal(s, rx);
    uint32_t eventos = 0, error = rx ? 8 : 4;
    uint8_t visitados[0x60000 / 32] = {0}; /* Un bit por dirección SRAM alineada. */
    uint32_t dir;
    size_t total = 0;

    if (!c || !cantidad) {
        return 0;
    }
    dir = 0x3fc00000u | (c->link & 0xfffff);
    /* START se consume una vez; PARK queda activo al terminar. El recorrido
     * está acotado aun con ciclos, descriptores vacíos y CHECK_OWNER apagado. */
    c->link = (c->link & 0xfffff) | (1u << (rx ? 24 : 23));
    for (unsigned nodos = 0; total < cantidad && nodos < 32768; nodos++) {
        uint8_t d[12];
        uint32_t ctrl, buf, sig, capacidad, largo;
        size_t n;

        c->state = dir & 0x3ffff;
        c->bfr_bfr_desc_addr = c->bfr_desc_addr;
        c->bfr_desc_addr = dir;
        if ((dir & 3) || !esp32c3_spi2_memoria(s, false, dir, d, sizeof(d))) {
            eventos |= error;
            break;
        }
        unsigned indice = (dir - ESP_GDMA_RAM_ADDR) / 4;
        if (visitados[indice / 8] & (1u << (indice % 8))) {
            eventos |= error;
            break;
        }
        visitados[indice / 8] |= 1u << (indice % 8);
        ctrl = ldl_le_p(d);
        buf = ldl_le_p(d + 4);
        sig = ldl_le_p(d + 8);
        c->desc_addr = sig;
        capacidad = ctrl & 0xfff;
        largo = (ctrl >> 12) & 0xfff;
        if (((c->conf1 & (1u << 12)) && !(ctrl & C3_DMA_OWNER)) ||
            (!rx && largo > capacidad) || !(rx ? capacidad : largo)) {
            eventos |= error;
            break;
        }
        n = MIN(rx ? capacidad : largo, cantidad - total);
        if (!esp32c3_spi2_memoria(s, rx, buf, datos + total, n)) {
            eventos |= error;
            break;
        }
        total += n;
        if (rx) {
            ctrl = (ctrl & ~(C3_DMA_OWNER | C3_DMA_EOF | 0xfff000u)) |
                   ((uint32_t)n << 12) | (total == cantidad ? C3_DMA_EOF : 0);
        } else if (n == largo && (c->conf0 & 4)) {
            ctrl &= ~C3_DMA_OWNER; /* OUT_AUTO_WRBACK */
        }
        if (rx || (n == largo && (c->conf0 & 4))) {
            stl_le_p(d, ctrl);
            if (!esp32c3_spi2_memoria(s, true, dir, d, 4)) {
                eventos |= error;
                break;
            }
        }
        /* RX devuelve el descriptor parcial al recibir EOF del periférico.
         * TX solo completa descriptor si transmite todos sus bytes: MS_DLEN
         * puede terminar antes, sin OUT_DONE, EOF ni devolución de OWNER. */
        if (rx || n == largo) {
            eventos |= 1; /* IN_DONE u OUT_DONE, en mapas genéricos separados. */
        }
        if (rx ? total == cantidad : ((ctrl & C3_DMA_EOF) && n == largo)) {
            eventos |= rx ? 2 : 10; /* IN_SUC_EOF / OUT_EOF|OUT_TOTAL_EOF */
            c->suc_eof_desc_addr = dir;
        }
        if (total == cantidad) {
            break;
        }
        if (!sig || (!rx && (ctrl & C3_DMA_EOF))) {
            eventos |= rx ? 16 : error; /* IN_DSCR_EMPTY */
            break;
        }
        dir = sig;
    }
    if (total < cantidad && !eventos) {
        eventos |= error;
    }
    if (total < cantidad && !(eventos & (error | (rx ? 16 : 0)))) {
        eventos |= error;
    }
    esp32c3_spi2_gdma_irq(c, eventos);
    return total;
}

static uint8_t esp32c3_spi2_byte(ESP32C3Spi2State *s, unsigned posicion)
{
    return s->registros[C3_SPI_W0 / 4 + posicion / 4] >> (8 * (posicion % 4));
}

static void esp32c3_spi2_transaccion(ESP32C3Spi2State *s)
{
    uint32_t user = s->registros[C3_SPI_USER / 4];
    uint32_t dma = s->registros[C3_SPI_DMA / 4];
    size_t largo = ((s->registros[C3_SPI_DLEN / 4] & 0x3ffff) + 8) / 8;
    size_t tx = (user & C3_SPI_TX) ? largo : 0;
    size_t rx = (user & C3_SPI_RX) ? largo : 0;
    unsigned inicio_tx = (user & (1u << 25)) ? 32 : 0;
    unsigned inicio_rx = (user & (1u << 24)) ? 32 : 0;
    bool dma_tx = dma & C3_SPI_DMA_TX;
    bool dma_rx = dma & C3_SPI_DMA_RX;
    bool duplex = user & 1;

    if (!dma_tx) { tx = MIN(tx, 64 - inicio_tx); }
    if (!dma_rx) { rx = MIN(rx, 64 - inicio_rx); }
    if (dma_rx && rx && !esp32c3_spi2_canal(s, true)) { rx = 0; }
    if (dma_rx && rx && (!dma_tx || !esp32c3_spi2_canal(s, false))) {
        tx = 0; /* RX full duplex sin outlink: clocks cero, nunca W0 antiguo. */
    }
    uint8_t *mosi = g_malloc0(MAX(tx, 1));
    uint8_t *miso = g_malloc0(MAX(rx, 1));

    /* Los eventos RAW quedan pendientes hasta CLR o escritura explícita de
     * RAW. USR no debe perder el fin de otra modalidad todavía sin atender. */
    if (dma_tx && tx) {
        size_t leidos = esp32c3_spi2_dma_copiar(s, false, mosi, tx);
        if (leidos != tx) { s->registros[C3_SPI_RAW / 4] |= 1u << 18; }
        tx = leidos;
    } else {
        for (unsigned i = 0; i < tx; i++) {
            mosi[i] = esp32c3_spi2_byte(s, inicio_tx + i);
        }
    }
    if (user & (1u << 31)) {
        uint32_t cmd = s->registros[0x18 / 4];
        unsigned n = (((cmd >> 28) & 15) + 8) / 8;
        for (unsigned i = 0; i < n; i++) { ssi_transfer(s->spi, (cmd >> (8 * i)) & 255); }
    }
    if (user & (1u << 30)) {
        unsigned n = (((s->registros[0x14 / 4] >> 27) & 31) + 8) / 8;
        for (unsigned i = 0; i < n; i++) {
            ssi_transfer(s->spi, (s->registros[1] >> (24 - 8 * i)) & 255);
        }
    }
    /* La trama lleva bytes MOSI, no ciclos dummy ni estados de CS. En RX sin
     * TX se generan clocks con MOSI cero, como en la prueba del SPI clásico. */
    size_t clocks = duplex ? MAX(tx, rx) : tx + rx;
    for (size_t i = 0; i < clocks; i++) {
        ssi_transfer(s->spi, i < tx ? mosi[i] : 0);
    }
    memset(miso, 0xff, rx);
    if (dma_rx && rx) {
        if (esp32c3_spi2_dma_copiar(s, true, miso, rx) != rx) {
            s->registros[C3_SPI_RAW / 4] |= 1u << 17;
        }
    } else {
        for (unsigned i = 0; i < rx; i++) {
            unsigned p = inicio_rx + i;
            s->registros[C3_SPI_W0 / 4 + p / 4] |= 255u << (8 * (p % 4));
        }
    }
    g_free(mosi);
    g_free(miso);
    taller_puente_spi_fin(ESP32C3_SPI2_BUS);
    s->registros[C3_SPI_RAW / 4] |=
        (s->registros[C3_SPI_SLAVE / 4] & (1u << 28)) ? C3_SPI_SEG_DONE : C3_SPI_DONE;
    esp32c3_spi2_irq(s);
}

static bool esp32c3_spi2_registro(hwaddr addr)
{
    return !(addr & 3) && (addr <= 0x40 ||
           (addr >= C3_SPI_W0 && addr <= 0xe8) || addr == 0xf0);
}

static uint64_t esp32c3_spi2_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3Spi2State *s = ESP32C3_SPI2(opaque);

    if (!esp32c3_spi2_registro(addr) || addr == C3_SPI_CMD || addr == C3_SPI_CLR) {
        return 0;
    }
    return s->registros[addr / 4];
}

static void esp32c3_spi2_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32C3Spi2State *s = ESP32C3_SPI2(opaque);
    uint32_t v = (uint32_t)value;

    if (!esp32c3_spi2_registro(addr) || addr == C3_SPI_ST) { return; }
    if (addr == C3_SPI_CMD) {
        /* UPDATE no transmite. Ambos pulsos terminan dentro del acceso MMIO. */
        if ((v & C3_SPI_USR) && !(s->registros[C3_SPI_SLAVE / 4] & (1u << 26))) {
            esp32c3_spi2_transaccion(s);
        }
    } else if (addr == C3_SPI_CLR) {
        s->registros[C3_SPI_RAW / 4] &= ~(v & C3_SPI_IRQ_MASK);
    } else {
        if (addr == C3_SPI_RAW || addr == C3_SPI_ENA) { v &= C3_SPI_IRQ_MASK; }
        if (addr == C3_SPI_DLEN) { v &= 0x3ffff; }
        if (addr == C3_SPI_DMA) { v &= 0x1fffffffu; } /* Resets AFIFO: pulsos. */
        if (addr == C3_SPI_SLAVE) { v &= ~(1u << 27); } /* SOFT_RESET: pulso. */
        s->registros[addr / 4] = v;
    }
    esp32c3_spi2_irq(s);
}

static const MemoryRegionOps esp32c3_spi2_ops = {
    .read = esp32c3_spi2_read,
    .write = esp32c3_spi2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_spi2_reiniciar(Object *obj, ResetType tipo)
{
    ESP32C3Spi2State *s = ESP32C3_SPI2(obj);

    memset(s->registros, 0, sizeof(s->registros));
    s->registros[0x20 / 4] = 0x3f; /* CS0..5 deshabilitados. */
    s->registros[0xf0 / 4] = 0x02007220;
    esp32c3_spi2_irq(s);
}

static void esp32c3_spi2_init(Object *obj)
{
    ESP32C3Spi2State *s = ESP32C3_SPI2(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_spi2_ops, s,
                          TYPE_ESP32C3_SPI2, ESP32C3_SPI2_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->spi = ssi_create_bus(DEVICE(s), "spi");
    esp32c3_spi2_reiniciar(obj, RESET_TYPE_COLD);
}

static void esp32c3_spi2_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = esp32c3_spi2_reiniciar;
}

static const TypeInfo esp32c3_spi2_info = {
    .name = TYPE_ESP32C3_SPI2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3Spi2State),
    .instance_init = esp32c3_spi2_init,
    .class_init = esp32c3_spi2_class_init,
};

static void esp32c3_spi2_register_types(void)
{
    type_register_static(&esp32c3_spi2_info);
}

type_init(esp32c3_spi2_register_types)
