/*
 * ESP32-C3 GDMA emulation
 * Copyright (c) 2023-2025 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 Taller.
 * GPL-2.0-or-later.
 * Traducción de registros C3 sobre el motor genérico de Espressif.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/dma/esp32c3_gdma.h"

/* Traducción C3: IN tiene cinco eventos, OUT cuatro. No dividir nueve por dos.
 * Las interrupciones avanzan 0x10 por canal; las configuraciones 0xc0. */
static const unsigned esp32c3_gdma_dir[] = {0, 0, 0, 1, 1, 0, 1, 0, 1};
static const unsigned esp32c3_gdma_bit[] = {0, 1, 2, 0, 1, 3, 2, 4, 3};

static uint32_t esp32c3_gdma_leer_irq(ESPGdmaState *s, unsigned canal, DmaRegister reg)
{
    uint32_t valor[2] = {
        esp_gdma_read_chan_register(s, ESP_GDMA_IN_IDX, canal, reg),
        esp_gdma_read_chan_register(s, ESP_GDMA_OUT_IDX, canal, reg),
    };
    uint32_t resultado = 0;

    for (unsigned b = 0; b < 9; b++) {
        if (valor[esp32c3_gdma_dir[b]] & (1u << esp32c3_gdma_bit[b])) {
            resultado |= 1u << b;
        }
    }
    return resultado;
}

static void esp32c3_gdma_escribir_irq(ESPGdmaState *s, unsigned canal,
                                     DmaRegister reg, uint32_t v)
{
    uint32_t valor[2] = {0, 0};

    for (unsigned b = 0; b < 9; b++) {
        if (v & (1u << b)) {
            valor[esp32c3_gdma_dir[b]] |= 1u << esp32c3_gdma_bit[b];
        }
    }
    for (unsigned dir = 0; dir < 2; dir++) {
        esp_gdma_write_chan_register(s, dir, canal, reg, valor[dir]);
    }
}

static DmaRegister esp32c3_gdma_registro(unsigned desplazamiento)
{
    static const DmaRegister registros[] = {
        GDMA_CONF0_REG, GDMA_CONF1_REG, GDMA_FIFO_ST_REG, GDMA_POP_REG,
        GDMA_LINK_REG, GDMA_STATE_REG, GDMA_SUC_EOF_DESC_REG,
        GDMA_ERR_EOF_DESC_REG, GDMA_DESC_ADDR_REG, GDMA_BF0_DESC_ADDR_REG,
        GDMA_BF1_DESC_ADDR_REG, GDMA_PRIORITY_REG, GDMA_PERI_SEL_REG,
    };
    return desplazamiento <= 0x30 ? registros[desplazamiento / 4] : GDMA_UNKNOWN;
}

static DmaRegister esp32c3_gdma_registro_irq(unsigned desplazamiento)
{
    static const DmaRegister registros[] = {
        GDMA_INT_RAW_REG, GDMA_INT_ST_REG, GDMA_INT_ENA_REG, GDMA_INT_CLR_REG,
    };
    return registros[(desplazamiento % DMA_INT_CHAN_REGS_SIZE) / 4];
}

static uint64_t esp32c3_gdma_read(void *opaque, hwaddr addr, unsigned size)
{
    ESPGdmaState *s = &ESP32C3_GDMA(opaque)->parent;

    if (addr & 3) { return 0; }
    if (addr <= 0x2c) {
        return esp32c3_gdma_leer_irq(s, addr / DMA_INT_CHAN_REGS_SIZE,
                                    esp32c3_gdma_registro_irq(addr));
    }
    if (addr == 0x44) { return esp_gdma_read_register(s, GDMA_MISC_REG); }
    if (addr >= 0x70 && addr <= 0x280) {
        unsigned bloque = (addr - 0x70) / DMA_DIR_REGS_SIZE;
        DmaRegister reg = esp32c3_gdma_registro((addr - 0x70) % DMA_DIR_REGS_SIZE);
        if (reg != GDMA_UNKNOWN) {
            return esp_gdma_read_chan_register(s, bloque & 1, bloque / 2, reg);
        }
    }
    return 0;
}

static void esp32c3_gdma_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESPGdmaState *s = &ESP32C3_GDMA(opaque)->parent;

    if (addr & 3) { return; }
    if (addr <= 0x2c) {
        esp32c3_gdma_escribir_irq(s, addr / DMA_INT_CHAN_REGS_SIZE,
                                  esp32c3_gdma_registro_irq(addr), value);
    } else if (addr == 0x44) {
        esp_gdma_write_register(s, GDMA_MISC_REG, value);
    } else if (addr >= 0x70 && addr <= 0x280) {
        unsigned bloque = (addr - 0x70) / DMA_DIR_REGS_SIZE;
        DmaRegister reg = esp32c3_gdma_registro((addr - 0x70) % DMA_DIR_REGS_SIZE);
        if (reg != GDMA_UNKNOWN) {
            unsigned dir = bloque & 1, canal = bloque / 2;
            uint32_t start = 1u << (dir ? 21 : 22);
            uint32_t stop = 1u << (dir ? 20 : 21);
            uint32_t park = 1u << (dir ? 23 : 24);
            if (reg == GDMA_LINK_REG) {
                if (value & stop) {
                    /* STOP es un pulso; no debe bloquear el próximo START. */
                    value = (value & (dir ? 0xfffff : 0x1fffff)) | park;
                } else if (value & start) {
                    value &= ~(uint64_t)park;
                }
            } else if (reg == GDMA_CONF0_REG && (value & 1)) {
                uint32_t link = esp_gdma_read_chan_register(s, dir, canal, GDMA_LINK_REG);
                esp_gdma_write_chan_register(s, dir, canal, GDMA_LINK_REG,
                    (link & (dir ? 0xfffff : 0x1fffff)) | park);
            }
            esp_gdma_write_chan_register(s, bloque & 1, bloque / 2, reg, value);
        }
    }
}

static const MemoryRegionOps esp_gdma_ops = {
    .read =  esp32c3_gdma_read,
    .write = esp32c3_gdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static void esp32c3_gdma_init(Object *obj)
{
    ESP32C3GdmaState *s = ESP32C3_GDMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp_gdma_ops, s,
                          TYPE_ESP32C3_GDMA, ESP32C3_GDMA_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

/**
 * @brief Check if the given GDMA peripheral is reserved on the ESP32-C3
 */
static bool esp32c3_is_periph_invalid(ESPGdmaState *s, GdmaPeripheral per)
{
    (void) s;
    /* On the ESP32-C3, some peripherals don't exist, mark them as reserved */
    return per == GDMA_SPI3 || per == GDMA_I2S1 || per == GDMA_LCDCAM;
}

static void esp32c3_gdma_class_init(ObjectClass *klass, void *data)
{
    ESPGdmaClass* class = ESP_GDMA_CLASS(klass);
    class->is_periph_invalid = esp32c3_is_periph_invalid;
    class->m_channel_count = ESP32C3_GDMA_CHANNEL_COUNT;
}


static const TypeInfo esp32c3_gdma_info = {
    .name = TYPE_ESP32C3_GDMA,
    .parent = TYPE_ESP_GDMA,
    .instance_size = sizeof(ESP32C3GdmaState),
    .instance_init = esp32c3_gdma_init,
    .class_init = esp32c3_gdma_class_init,
    .class_size = sizeof(ESP32C3GdmaClass)
};


static void esp32c3_gdma_register_types(void)
{
    type_register_static(&esp32c3_gdma_info);
}


type_init(esp32c3_gdma_register_types)
