/*
 * Puente de Taller (ver include/hw/misc/taller_puente.h para el protocolo).
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/ssi/ssi.h"
#include "hw/qdev-properties.h"
#include "hw/misc/taller_puente.h"
#include "hw/gpio/esp32_gpio.h"

#define CARGA_MAX_SPI 4096
#define BUSES_SPI 4
#define CABECERA 12

static CharBackend puente_fe;
static bool puente_activo;

/* Tramas que llegan del programa externo (protocolo v2): se acumulan hasta tener una entera. */
static uint8_t entrada_buf[CABECERA + 256];
static size_t entrada_len;

static void procesar_trama_entrante(const uint8_t *t, uint16_t largo)
{
    if (t[0] == TALLER_TRAMA_ENTRADA && largo >= 2) {
        esp32_gpio_fijar_entrada(t[CABECERA], t[CABECERA + 1] != 0);
    }
    /* otros tipos: se ignoran (compatibilidad con versiones futuras) */
}

static int puente_puede_leer(void *opaque)
{
    return sizeof(entrada_buf) - entrada_len;
}

static void puente_leer(void *opaque, const uint8_t *buf, int size)
{
    memcpy(entrada_buf + entrada_len, buf, size);
    entrada_len += size;
    while (entrada_len >= CABECERA) {
        uint16_t largo = lduw_le_p(entrada_buf + 2);
        size_t total = CABECERA + largo;
        if (total > sizeof(entrada_buf)) {
            entrada_len = 0; /* trama imposible: se descarta todo */
            return;
        }
        if (entrada_len < total) {
            return;
        }
        procesar_trama_entrante(entrada_buf, largo);
        memmove(entrada_buf, entrada_buf + total, entrada_len - total);
        entrada_len -= total;
    }
}

/* Por bus: [0] = número de bus, [1..] = bytes MOSI acumulados de la transacción en curso. */
static uint8_t spi_buf[BUSES_SPI][1 + CARGA_MAX_SPI];
static uint16_t spi_len[BUSES_SPI];

static void enviar(uint8_t tipo, const uint8_t *carga, uint16_t largo)
{
    uint8_t cab[12];
    cab[0] = tipo;
    cab[1] = TALLER_PUENTE_VERSION;
    stw_le_p(cab + 2, largo);
    stq_le_p(cab + 4, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    /* Bloqueante a propósito: el receptor (Taller) siempre está leyendo. */
    qemu_chr_fe_write_all(&puente_fe, cab, sizeof(cab));
    if (largo) {
        qemu_chr_fe_write_all(&puente_fe, carga, largo);
    }
}

void taller_puente_iniciar(Chardev *chr)
{
    uint8_t version[4];
    qemu_chr_fe_init(&puente_fe, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&puente_fe, puente_puede_leer, puente_leer, NULL, NULL, NULL, NULL, true);
    puente_activo = true;
    stl_le_p(version, TALLER_PUENTE_VERSION);
    enviar(TALLER_TRAMA_HOLA, version, sizeof(version));
}

bool taller_puente_activo(void)
{
    return puente_activo;
}

void taller_puente_spi_fin(uint8_t bus)
{
    if (!puente_activo || bus >= BUSES_SPI || spi_len[bus] == 0) {
        return;
    }
    spi_buf[bus][0] = bus;
    enviar(TALLER_TRAMA_SPI, spi_buf[bus], 1 + spi_len[bus]);
    spi_len[bus] = 0;
}

void taller_puente_spi_byte(uint8_t bus, uint8_t mosi)
{
    if (!puente_activo || bus >= BUSES_SPI) {
        return;
    }
    if (spi_len[bus] == CARGA_MAX_SPI) {
        taller_puente_spi_fin(bus); /* trama llena: se envía y se sigue acumulando */
    }
    spi_buf[bus][1 + spi_len[bus]++] = mosi;
}

void taller_puente_gpio(uint64_t niveles, uint64_t salidas)
{
    uint8_t carga[16];
    if (!puente_activo) {
        return;
    }
    for (int b = 0; b < BUSES_SPI; b++) {
        taller_puente_spi_fin(b);
    }
    stq_le_p(carga, niveles);
    stq_le_p(carga + 8, salidas);
    enviar(TALLER_TRAMA_GPIO, carga, sizeof(carga));
}

/* ---- Periférico SSI que cuelga de los buses SPI2/SPI3 y reenvía cada byte al puente ---- */

typedef struct TallerSpiState {
    SSIPeripheral parent_obj;
    uint8_t bus;
} TallerSpiState;

OBJECT_DECLARE_SIMPLE_TYPE(TallerSpiState, TALLER_SPI)

static uint32_t taller_spi_transfer(SSIPeripheral *dev, uint32_t val)
{
    TallerSpiState *s = TALLER_SPI(dev);
    taller_puente_spi_byte(s->bus, (uint8_t)val);
    return 0xFF; /* MISO: línea en reposo (las respuestas síncronas llegan en la fase 3) */
}

static void taller_spi_realize(SSIPeripheral *dev, Error **errp)
{
}

static Property taller_spi_properties[] = {
    DEFINE_PROP_UINT8("bus", TallerSpiState, bus, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void taller_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = taller_spi_realize;
    k->transfer = taller_spi_transfer;
    k->cs_polarity = SSI_CS_NONE; /* el CS lo maneja el firmware por GPIO */
    device_class_set_props(dc, taller_spi_properties);
}

static const TypeInfo taller_spi_info = {
    .name = TYPE_TALLER_SPI,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(TallerSpiState),
    .class_init = taller_spi_class_init,
};

static void taller_puente_register_types(void)
{
    type_register_static(&taller_spi_info);
}

type_init(taller_puente_register_types)
