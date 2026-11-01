/*
 * Espejo de registros I2C de Taller, alimentado por el puente v4.
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/i2c/taller_i2c.h"
#include "hw/misc/taller_puente.h"

typedef struct TallerI2CDireccion {
    bool activo;
    uint8_t direccion;
    uint8_t espejo[TALLER_I2C_REGISTROS];
    uint8_t puntero;
    bool escritura;
    bool espera_puntero;
    uint8_t datos[TALLER_I2C_ESCRITURA_MAX];
    size_t largo;
} TallerI2CDireccion;

typedef struct TallerI2CState {
    I2CSlave parent_obj;
    uint8_t bus;
    TallerI2CDireccion direcciones[TALLER_I2C_DIRECCIONES];
    TallerI2CDireccion *actual;
} TallerI2CState;

OBJECT_DECLARE_SIMPLE_TYPE(TallerI2CState, TALLER_I2C)

static TallerI2CState *buses[TALLER_I2C_BUSES];

static TallerI2CDireccion *buscar(TallerI2CState *s, uint8_t direccion)
{
    for (int i = 0; i < TALLER_I2C_DIRECCIONES; i++) {
        TallerI2CDireccion *d = &s->direcciones[i];

        if (d->activo && d->direccion == direccion) {
            return d;
        }
    }
    return NULL;
}

static void vaciar(TallerI2CState *s, TallerI2CDireccion *d)
{
    if (d && d->escritura && d->largo) {
        taller_puente_i2c_escritura(s->bus, d->direccion, d->datos, d->largo);
        d->largo = 0;
    }
}

/*
 * QEMU 9.2 ofrece match_and_add: un esclavo por bus puede seleccionar uno de
 * varios espejos sin crear/destruir QOM durante la ejecución. La coincidencia
 * predeterminada solo compara I2CSlave.address; esta variante añade el nodo
 * únicamente para direcciones registradas, dejando las demás sin ACK. No
 * aceptamos general call. El controlador ESP32 llama i2c_end_transfer en cada
 * RSTART, por lo que vuelve a resolver incluso un cambio de dirección; el core
 * genérico conserva current_devs en un START repetido sin FINISH.
 */
static bool taller_i2c_match(I2CSlave *dev, uint8_t direccion, bool broadcast,
                            I2CNodeList *current_devs)
{
    TallerI2CState *s = TALLER_I2C(dev);
    TallerI2CDireccion *d = buscar(s, direccion);
    I2CNode *nodo;

    if (broadcast || !d) {
        return false;
    }
    s->actual = d;
    i2c_slave_set_address(dev, direccion); /* Dirección efectiva en las trazas. */
    nodo = g_new(I2CNode, 1);
    nodo->elt = dev;
    QLIST_INSERT_HEAD(current_devs, nodo, next);
    return true;
}

static int taller_i2c_event(I2CSlave *dev, enum i2c_event evento)
{
    TallerI2CState *s = TALLER_I2C(dev);
    TallerI2CDireccion *d = s->actual;

    switch (evento) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        vaciar(s, d);
        if (!d || !d->activo) {
            return 1;
        }
        d->escritura = evento == I2C_START_SEND;
        d->espera_puntero = d->escritura;
        break;
    case I2C_FINISH:
        vaciar(s, d);
        if (d) {
            d->escritura = false;
        }
        s->actual = NULL;
        break;
    case I2C_START_SEND_ASYNC:
        return 1; /* El ESP32 usa transferencias síncronas. */
    case I2C_NACK:
        /* NACK del maestro al último byte leído; el puntero ya avanzó. */
        break;
    }
    return 0;
}

static int taller_i2c_send(I2CSlave *dev, uint8_t dato)
{
    TallerI2CState *s = TALLER_I2C(dev);
    TallerI2CDireccion *d = s->actual;

    if (!d || !d->activo || !d->escritura) {
        return 1;
    }
    /* Más de 4 KiB: drenar un bloque sin perder el resto ni otro puntero. */
    if (d->largo == sizeof(d->datos)) {
        vaciar(s, d);
    }
    d->datos[d->largo++] = dato;
    if (d->espera_puntero) {
        d->puntero = dato;
        d->espera_puntero = false;
    } else {
        d->espejo[d->puntero++] = dato;
    }
    return 0;
}

static uint8_t taller_i2c_recv(I2CSlave *dev)
{
    TallerI2CDireccion *d = TALLER_I2C(dev)->actual;

    if (!d || !d->activo) {
        return 0xff;
    }
    return d->espejo[d->puntero++];
}

void taller_i2c_dispositivo(uint8_t bus, uint8_t direccion, bool activo)
{
    TallerI2CState *s;
    TallerI2CDireccion *d;

    if (bus >= TALLER_I2C_BUSES || !buses[bus] ||
        direccion == 0 || direccion > 0x7f) {
        return;
    }
    s = buses[bus];
    d = buscar(s, direccion);
    if (d) {
        if (!activo) {
            vaciar(s, d);
            if (s->actual == d) {
                s->actual = NULL;
            }
            memset(d, 0, sizeof(*d));
        }
        return;
    }
    if (activo) {
        for (int i = 0; i < TALLER_I2C_DIRECCIONES; i++) {
            d = &s->direcciones[i];
            if (!d->activo) {
                d->activo = true;
                d->direccion = direccion;
                return;
            }
        }
    }
}

void taller_i2c_registros(uint8_t bus, uint8_t direccion, uint8_t registro,
                         const uint8_t *datos, size_t largo)
{
    TallerI2CDireccion *d;

    if (bus >= TALLER_I2C_BUSES || !buses[bus]) {
        return;
    }
    d = buscar(buses[bus], direccion);
    if (d) {
        for (size_t i = 0; i < largo; i++) {
            d->espejo[registro++] = datos[i];
        }
    }
}

void taller_i2c_preparar(I2CBus *bus, uint8_t numero)
{
    TallerI2CState *s;
    I2CSlave *dev;

    assert(numero < TALLER_I2C_BUSES && !buses[numero]);
    dev = i2c_slave_new(TYPE_TALLER_I2C, 0);
    s = TALLER_I2C(dev);
    s->bus = numero;
    i2c_slave_realize_and_unref(dev, bus, &error_fatal);
    buses[numero] = s;
}

static void taller_i2c_unrealize(DeviceState *dev)
{
    TallerI2CState *s = TALLER_I2C(dev);

    if (buses[s->bus] == s) {
        buses[s->bus] = NULL;
    }
}

static void taller_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    /* Requiere el número de bus y el registro global que prepara el SoC. */
    dc->user_creatable = false;
    dc->unrealize = taller_i2c_unrealize;
    sc->match_and_add = taller_i2c_match;
    sc->event = taller_i2c_event;
    sc->send = taller_i2c_send;
    sc->recv = taller_i2c_recv;
}

static const TypeInfo taller_i2c_info = {
    .name = TYPE_TALLER_I2C,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TallerI2CState),
    .class_init = taller_i2c_class_init,
};

static void taller_i2c_register_types(void)
{
    type_register_static(&taller_i2c_info);
}

type_init(taller_i2c_register_types)
