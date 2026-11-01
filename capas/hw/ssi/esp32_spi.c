/*
 * ESP32 SPI controller
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/esp32_spi.h"
#include "hw/misc/taller_puente.h"
#include "hw/misc/esp32_flash_enc.h"



enum {
    CMD_RES = 0xab,
    CMD_DP = 0xb9,
    CMD_CE = 0x60,
    CMD_BE = 0xD8,
    CMD_SE = 0x20,
    CMD_PP = 0x02,
    CMD_WRSR = 0x1,
    CMD_RDSR = 0x5,
    CMD_RDID = 0x9f,
    CMD_WRDI = 0x4,
    CMD_WREN = 0x6,
    CMD_READ = 0x03,
};


#define ESP32_SPI_REG_SIZE    0x1000

static void esp32_spi_do_command(Esp32SpiState* state, uint32_t cmd_reg);

static void esp32_spi_update_irq(Esp32SpiState *s)
{
    /* SLAVE tiene cinco estados y sus habilitaciones cinco bits más arriba.
     * trans_done es R/W: IDF también lo pone a uno para despertar su ISR. */
    qemu_set_irq(s->irq, (s->slave_reg & (s->slave_reg >> 5) & 0x1f) != 0);
    qemu_set_irq(s->dma_irq, (s->dma.raw & s->dma.ena) != 0);
}

/* Diagnóstico: TALLER_SPI_TRAZA=1 vuelca a stderr los accesos a los SPI2/SPI3 (no al 0/1 de la flash). */
static bool esp32_spi_traza(Esp32SpiState *s)
{
    static int activa = -1;
    if (activa < 0) {
        activa = getenv("TALLER_SPI_TRAZA") != NULL;
    }
    return activa && s->index >= 2;
}

static uint64_t esp32_spi_leer(void *opaque, hwaddr addr, unsigned int size);

static uint64_t esp32_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint64_t r = esp32_spi_leer(opaque, addr, size);
    if (esp32_spi_traza(ESP32_SPI(opaque))) {
        fprintf(stderr, "spi%d L %03x = %08llx\n", ESP32_SPI(opaque)->index, (unsigned)addr, (unsigned long long)r);
    }
    return r;
}

static uint64_t esp32_spi_leer(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SpiState *s = ESP32_SPI(opaque);
    uint64_t r = 0;
    if (addr >= 0x100 && addr <= 0x14c) {
        return taller_spi_dma_leer(&s->dma, addr);
    }
    switch (addr) {
    case A_SPI_CMD:
        /* Las operaciones se completan síncronamente. USR se autolimpia. */
        break;
    case A_SPI_CLOCK:
        r = s->clock_reg;
        break;
    case A_SPI_ADDR:
        r = s->addr_reg;
        break;
    case A_SPI_CTRL:
        r = s->ctrl_reg;
        break;
    case A_SPI_STATUS:
        r = s->status_reg;
        break;
    case A_SPI_CTRL1:
        r = s->ctrl1_reg;
        break;
    case A_SPI_CTRL2:
        r = s->ctrl2_reg;
        break;
    case A_SPI_USER:
        r = s->user_reg;
        break;
    case A_SPI_USER1:
        r = s->user1_reg;
        break;
    case A_SPI_USER2:
        r = s->user2_reg;
        break;
    case A_SPI_MOSI_DLEN:
        r = s->mosi_dlen_reg;
        break;
    case A_SPI_MISO_DLEN:
        r = s->miso_dlen_reg;
        break;
    case A_SPI_PIN:
        r = s->pin_reg;
        break;
    case A_SPI_W0 ... A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t):
        r = s->data_reg[(addr - A_SPI_W0) / sizeof(uint32_t)];
        break;
    case A_SPI_EXT2:
        r = 0;
        break;
    case A_SPI_SLAVE:
        r = s->slave_reg;
        break;
    }
    return r;
}

static void esp32_spi_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32SpiState *s = ESP32_SPI(opaque);
    if (esp32_spi_traza(s)) {
        fprintf(stderr, "spi%d E %03x <- %08llx\n", s->index, (unsigned)addr, (unsigned long long)value);
    }
    if (addr >= 0x100 && addr <= 0x14c) {
        taller_spi_dma_escribir(&s->dma, addr, value);
        esp32_spi_update_irq(s);
        return;
    }
    switch (addr) {
    case A_SPI_CLOCK:
        s->clock_reg = value;
        break;
    case A_SPI_SLAVE:
        s->slave_reg = value & ~BIT(31); /* sync_reset autolimpiable */
        esp32_spi_update_irq(s);
        break;
    case A_SPI_W0 ... A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t):
        s->data_reg[(addr - A_SPI_W0) / sizeof(uint32_t)] = value;
        break;
    case A_SPI_ADDR:
        s->addr_reg = value;
        break;
    case A_SPI_CTRL:
        s->ctrl_reg = value;
        break;
    case A_SPI_STATUS:
        s->status_reg = value;
        break;
    case A_SPI_CTRL1:
        s->ctrl1_reg = value;
        break;
    case A_SPI_CTRL2:
        s->ctrl2_reg = value;
        break;
    case A_SPI_USER:
        s->user_reg = value;
        break;
    case A_SPI_USER1:
        s->user1_reg = value;
        break;
    case A_SPI_USER2:
        s->user2_reg = value;
        break;
    case A_SPI_MOSI_DLEN:
        s->mosi_dlen_reg = value & 0xffffff;
        break;
    case A_SPI_MISO_DLEN:
        s->miso_dlen_reg = value & 0xffffff;
        break;
    case A_SPI_PIN:
        s->pin_reg = value;
        break;
    case A_SPI_CMD:
        esp32_spi_do_command(s, value);
        break;
    }
}

typedef struct Esp32SpiTransaction {
    int cmd_bytes;
    uint32_t cmd;
    int addr_bytes;
    uint32_t addr;
    int data_tx_bytes;
    int data_rx_bytes;
    uint32_t* data;
} Esp32SpiTransaction;

static void esp32_spi_txrx_buffer(Esp32SpiState *s, void *buf, int tx_bytes, int rx_bytes)
{
    int bytes = MAX(tx_bytes, rx_bytes);
    uint8_t *c_buf = (uint8_t*) buf;
    for (int i = 0; i < bytes; ++i) {
        uint8_t byte = 0;
        if (i < tx_bytes) {
            memcpy(&byte, c_buf + i, 1);
        }
        uint32_t res = ssi_transfer(s->spi, byte);
        if (i < rx_bytes) {
            memcpy(c_buf + i, &res, 1);
        }
    }
}

static void esp32_spi_cs_set(Esp32SpiState *s, int value)
{
    for (int i = 0; i < ESP32_SPI_CS_COUNT; ++i) {
        qemu_set_irq(s->cs_gpio[i], ((s->pin_reg & (1 << i)) == 0) ? value : 1);
    }
}

static void esp32_spi_transaction(Esp32SpiState *s, Esp32SpiTransaction *t)
{
    esp32_spi_cs_set(s, 0);
    esp32_spi_txrx_buffer(s, &t->cmd, t->cmd_bytes, 0);
    esp32_spi_txrx_buffer(s, &t->addr, t->addr_bytes, 0);
    esp32_spi_txrx_buffer(s, t->data, t->data_tx_bytes, t->data_rx_bytes);
    esp32_spi_cs_set(s, 1);
    taller_puente_spi_fin(s->index); /* Taller: una trama SPI por transacción */
}

/* Convert one of the hardware "bitlen" registers to a byte count */
static inline int bitlen_to_bytes(uint32_t val)
{
    return (val + 1 + 7) / 8; /* bitlen registers hold number of bits, minus one */
}

static void maybe_encrypt_data(Esp32SpiState *s)
{
    Esp32FlashEncryptionState* flash_enc = esp32_flash_encryption_find();
    if (esp32_flash_encryption_enabled(flash_enc)) {
        esp32_flash_encryption_get_result(flash_enc, &s->data_reg[0], 8);
    }
}

static bool esp32_spi_dma_read(void *opaque, uint32_t addr, void *buf, size_t n)
{
    return address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                              buf, n) == MEMTX_OK;
}

static bool esp32_spi_dma_write(void *opaque, uint32_t addr, const void *buf, size_t n)
{
    return address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                               buf, n) == MEMTX_OK;
}

static void esp32_spi_dma_transaction(Esp32SpiState *s, Esp32SpiTransaction *t)
{
    TallerSpiDmaMemoria mem = {s, esp32_spi_dma_read, esp32_spi_dma_write};
    bool tx_dma = s->dma.out_active;
    bool rx_dma = s->dma.in_active;
    bool duplex = FIELD_EX32(s->user_reg, SPI_USER, DOUTDIN);
    int tx = t->data_tx_bytes;
    int rx = t->data_rx_bytes;
    int n = MAX(tx, rx);
    g_autofree uint8_t *mosi = g_malloc0(MAX(n, 1));
    g_autofree uint8_t *miso = g_malloc0(MAX(rx, 1));

    if (tx_dma && tx) {
        tx = taller_spi_dma_copiar(&s->dma, &mem, false, mosi, tx);
    } else if (!tx_dma && tx) {
        /* RX sin outlink: el hardware genera clocks con MOSI a cero; no leer
         * más allá de W15 cuando IDF activa MOSI para una lectura full duplex. */
        memcpy(mosi, s->data_reg, MIN(tx, (int)sizeof(s->data_reg)));
    }
    esp32_spi_cs_set(s, 0);
    esp32_spi_txrx_buffer(s, &t->cmd, t->cmd_bytes, 0);
    esp32_spi_txrx_buffer(s, &t->addr, t->addr_bytes, 0);
    n = duplex ? MAX(tx, rx) : tx + rx;
    for (int i = 0; i < n; i++) {
        uint8_t v = i < tx ? mosi[i] : 0;
        uint8_t respuesta = ssi_transfer(s->spi, v);
        int ri = duplex ? i : i - tx;
        /* SSI sin ningún periférico devuelve cero. Línea MISO suelta = 0xff.
         * Con puente, taller.spi ya devuelve 0xff y observa todos los clocks. */
        if (QTAILQ_EMPTY(&BUS(s->spi)->children)) {
            respuesta = 0xff;
        }
        if (ri >= 0 && ri < rx) {
            miso[ri] = respuesta;
        }
    }
    if (rx_dma && rx) {
        taller_spi_dma_copiar(&s->dma, &mem, true, miso, rx);
    } else if (rx) {
        memcpy(s->data_reg, miso, MIN(rx, (int)sizeof(s->data_reg)));
    }
    /* IDF puede arrancar inlink con NULL como workaround durante TX sin RX.
     * Sin fase MISO no se toca ese descriptor ni se inventa un error. */
    s->dma.out_active = false;
    s->dma.in_active = false;
    esp32_spi_cs_set(s, 1);
    taller_puente_spi_fin(s->index);
}

static void esp32_spi_do_command(Esp32SpiState* s, uint32_t cmd_reg)
{
    Esp32SpiTransaction t = {
        .cmd_bytes = 1
    };
    switch (cmd_reg) {
    case R_SPI_CMD_READ_MASK:
        t.cmd = CMD_READ;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> (32 - t.addr_bytes * 8);
        t.data = &s->data_reg[0];
        t.data_rx_bytes = bitlen_to_bytes(s->miso_dlen_reg);
        break;

    case R_SPI_CMD_WREN_MASK:
        t.cmd = CMD_WREN;
        break;

    case R_SPI_CMD_WRDI_MASK:
        t.cmd = CMD_WRDI;
        break;

    case R_SPI_CMD_RDID_MASK:
        t.cmd = CMD_RDID;
        t.data = &s->data_reg[0];
        t.data_rx_bytes = 3;
        break;

    case R_SPI_CMD_RDSR_MASK:
        t.cmd = CMD_RDSR;
        t.data = &s->status_reg;
        t.data_rx_bytes = 1;
        break;

    case R_SPI_CMD_WRSR_MASK:
        t.cmd = CMD_WRSR;
        t.data = &s->status_reg;
        t.data_tx_bytes = 1;
        break;

    case R_SPI_CMD_PP_MASK:
        maybe_encrypt_data(s);
        t.cmd = CMD_PP;
        t.data = &s->data_reg[0];
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> 8;
        t.data = &s->data_reg[0];
        t.data_tx_bytes = s->addr_reg >> 24;
        break;

    case R_SPI_CMD_SE_MASK:
        t.cmd = CMD_SE;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> (32 - t.addr_bytes * 8);
        break;

    case R_SPI_CMD_BE_MASK:
        t.cmd = CMD_BE;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> (32 - t.addr_bytes * 8);
        break;

    case R_SPI_CMD_CE_MASK:
        t.cmd = CMD_CE;
        break;

    case R_SPI_CMD_DP_MASK:
        t.cmd = CMD_DP;
        break;

    case R_SPI_CMD_RES_MASK:
        t.cmd = CMD_RES;
        t.data = &s->data_reg[0];
        t.data_rx_bytes = 3;
        break;

    case R_SPI_CMD_USR_MASK:
        maybe_encrypt_data(s);
        /* Taller: en SPI2/SPI3 (propósito general) la fase de comando solo existe si el bit
         * USR_COMMAND está activo, como en el hardware. La segunda condición es un apaño
         * de Espressif para la flash (SPI0/SPI1) y en SPI2/3 añadía un byte 0x00 por transacción. */
        if (FIELD_EX32(s->user_reg, SPI_USER, COMMAND) ||
            (s->index < 2 && FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_BITLEN))) {
            t.cmd = FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_VALUE);
            t.cmd_bytes = bitlen_to_bytes(FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_BITLEN));
        } else {
            t.cmd_bytes = 0;
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, ADDR)) {
            t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
            t.addr = bswap32(s->addr_reg);
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, MOSI)) {
            t.data = &s->data_reg[0];
            t.data_tx_bytes = bitlen_to_bytes(s->mosi_dlen_reg);
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, MISO)) {
            t.data = &s->data_reg[0];
            t.data_rx_bytes = bitlen_to_bytes(s->miso_dlen_reg);
        }
        break;
    default:
        return;
    }
    s->slave_reg &= ~R_SPI_SLAVE_TRANS_DONE_MASK;
    esp32_spi_update_irq(s);
    if (cmd_reg == R_SPI_CMD_USR_MASK && s->dma_chan_sel &&
        taller_spi_dma_canal(*s->dma_chan_sel, s->index) &&
        (s->dma.out_active || s->dma.in_active)) {
        esp32_spi_dma_transaction(s, &t);
    } else {
        /* W0..W15 nunca puede albergar una transferencia DMA larga. */
        if (t.data == s->data_reg) {
            t.data_tx_bytes = MIN(t.data_tx_bytes, (int)sizeof(s->data_reg));
            t.data_rx_bytes = MIN(t.data_rx_bytes, (int)sizeof(s->data_reg));
        }
        esp32_spi_transaction(s, &t);
    }
    s->slave_reg |= R_SPI_SLAVE_TRANS_DONE_MASK;
    esp32_spi_update_irq(s);
}


static const MemoryRegionOps esp32_spi_ops = {
    .read =  esp32_spi_read,
    .write = esp32_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_spi_reset_hold(Object *obj, ResetType type)
{
    Esp32SpiState *s = ESP32_SPI(obj);
    s->pin_reg = 0x6;
    s->user1_reg = FIELD_DP32(0, SPI_USER1, ADDR_BITLEN, 23);
    s->user1_reg = FIELD_DP32(s->user1_reg, SPI_USER1, DUMMY_CYCLELEN, 7);
    s->user2_reg = FIELD_DP32(0, SPI_USER2, COMMAND_BITLEN, 4);
    s->user2_reg = FIELD_DP32(s->user2_reg, SPI_USER2, COMMAND_VALUE, 0);
    s->status_reg = 0;
    s->addr_reg = s->ctrl_reg = s->ctrl1_reg = s->ctrl2_reg = 0;
    s->user_reg = s->mosi_dlen_reg = s->miso_dlen_reg = 0;
    s->clock_reg = s->slave_reg = 0;
    memset(s->data_reg, 0, sizeof(s->data_reg));
    memset(&s->dma, 0, sizeof(s->dma));
    esp32_spi_update_irq(s);
}

static void esp32_spi_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_spi_init(Object *obj)
{
    Esp32SpiState *s = ESP32_SPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_spi_ops, s,
                          TYPE_ESP32_SPI, ESP32_SPI_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->dma_irq);

    s->spi = ssi_create_bus(DEVICE(s), "spi");
    qdev_init_gpio_out_named(DEVICE(s), &s->cs_gpio[0], SSI_GPIO_CS, ESP32_SPI_CS_COUNT);
}

static Property esp32_spi_properties[] = {
    DEFINE_PROP_UINT8("index", Esp32SpiState, index, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_spi_reset_hold;
    dc->realize = esp32_spi_realize;
    device_class_set_props(dc, esp32_spi_properties);
}

static const TypeInfo esp32_spi_info = {
    .name = TYPE_ESP32_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SpiState),
    .instance_init = esp32_spi_init,
    .class_init = esp32_spi_class_init
};

static void esp32_spi_register_types(void)
{
    type_register_static(&esp32_spi_info);
}

type_init(esp32_spi_register_types)
