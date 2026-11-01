/*
 * GPIO ESP32-S3 para Taller, basado en la capa C3 y el modelo de Espressif.
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 Taller. GPL-2.0-or-later.
 * Dos bancos para GPIO0..21 y GPIO26..48, PAD_DRIVER, matriz de 9 bits y
 * trenes armados. IO_MUX, inversión, IRQ y filtros no están emulados.
 * Entradas externas inicialmente altas; STRAP es una propiedad independiente.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/misc/taller_puente.h"
#include "hw/misc/esp32c3_ledc.h"

static bool esp32s3_gpio_pin_valido(unsigned pin)
{
    return pin < ESP32S3_GPIO_PINES && (pin < 22 || pin >= 26);
}

void esp32s3_gpio_fijar_entrada(ESP32S3GPIOState *s, uint8_t pin, bool nivel)
{
    if (!esp32s3_gpio_pin_valido(pin)) {
        return;
    }
    if (nivel) {
        s->in_externo |= 1ull << pin;
    } else {
        s->in_externo &= ~(1ull << pin);
    }
}

void esp32s3_gpio_armar_entrada(ESP32S3GPIOState *s, const uint8_t *datos,
                               size_t largo)
{
    /* Validar ambos pines antes de sustituir un tren, incluidos los huecos. */
    if (largo >= 9 && esp32s3_gpio_pin_valido(datos[0]) &&
        esp32s3_gpio_pin_valido(datos[6])) {
        taller_armado_recibir(&s->armado, datos, largo);
    }
}

int esp32s3_gpio_pin_de_senal(void *opaque, int senal)
{
    ESP32S3GPIOState *s = opaque;

    if (s && senal >= 0 && senal < ESP32S3_GPIO_SENAL_SOFTWARE) {
        for (unsigned pin = 0; pin < ESP32S3_GPIO_PINES; pin++) {
            if (esp32s3_gpio_pin_valido(pin) &&
                (s->seleccion_salida[pin] & 0x1ff) == (unsigned)senal) {
                return pin; /* Como el clásico: primera ruta si hay varias. */
            }
        }
    }
    return -1;
}

static void esp32s3_gpio_paso_armado(void *opaque)
{
    ESP32S3GpioTren *t = opaque;
    TallerArmadoRanura *r = &t->gpio->armado.ranuras[t->indice];
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t pin;
    bool nivel;

    while (taller_armado_avanzar(r, ahora_ns, &pin, &nivel)) {
        esp32s3_gpio_fijar_entrada(t->gpio, pin, nivel);
    }
    if (r->sonando) {
        timer_mod_ns(t->timer, r->proximo_ns);
    }
}

static void esp32s3_gpio_observar_armados(ESP32S3GPIOState *s)
{
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t disparos = taller_armado_observar(&s->armado, s->out, s->enable,
                                             ahora_ns);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        if (disparos & (1u << i)) {
            taller_puente_disparo(s->armado.ranuras[i].en_curso.pin_disparo,
                                  ahora_ns);
            esp32s3_gpio_paso_armado(&s->trenes[i]);
        }
    }
}

static uint64_t esp32s3_gpio_entrada(ESP32S3GPIOState *s)
{
    uint64_t od = s->enable & s->drenador_abierto;

    return ((s->out & s->enable & ~od) | (s->out & s->in_externo & od) |
            (s->in_externo & ~s->enable)) & ESP32S3_GPIO_MASCARA;
}

static bool esp32s3_gpio_en_tabla(hwaddr addr, hwaddr base)
{
    return addr >= base && addr < base + 4 * ESP32S3_GPIO_PINES &&
           !(addr & 3) && esp32s3_gpio_pin_valido((addr - base) / 4);
}

static uint64_t esp32s3_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);

    if (esp32s3_gpio_en_tabla(addr, A_ESP32S3_GPIO_PIN0)) {
        return s->registro_pin[(addr - A_ESP32S3_GPIO_PIN0) / 4];
    }
    if (esp32s3_gpio_en_tabla(addr, A_ESP32S3_GPIO_FUNC0_OUT_SEL_CFG)) {
        return s->seleccion_salida[(addr - A_ESP32S3_GPIO_FUNC0_OUT_SEL_CFG) / 4];
    }
    switch (addr) {
    case A_ESP32S3_GPIO_OUT: return (uint32_t)s->out;
    case A_ESP32S3_GPIO_OUT1: return s->out >> 32;
    case A_ESP32S3_GPIO_ENABLE: return (uint32_t)s->enable;
    case A_ESP32S3_GPIO_ENABLE1: return s->enable >> 32;
    case A_ESP32S3_GPIO_IN: return (uint32_t)esp32s3_gpio_entrada(s);
    case A_ESP32S3_GPIO_IN1: return esp32s3_gpio_entrada(s) >> 32;
    case A_ESP32S3_GPIO_STRAP: return s->strap_mode;
    default: return 0;
    }
}

static void esp32s3_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);
    uint32_t v = value;
    uint64_t out_antes = s->out, enable_antes = s->enable;

    if (esp32s3_gpio_en_tabla(addr, A_ESP32S3_GPIO_PIN0)) {
        unsigned pin = (addr - A_ESP32S3_GPIO_PIN0) / 4;

        s->registro_pin[pin] = v;
        if (v & ESP32S3_GPIO_PAD_DRIVER) {
            s->drenador_abierto |= 1ull << pin;
        } else {
            s->drenador_abierto &= ~(1ull << pin);
        }
        return;
    }
    if (esp32s3_gpio_en_tabla(addr, A_ESP32S3_GPIO_FUNC0_OUT_SEL_CFG)) {
        unsigned pin = (addr - A_ESP32S3_GPIO_FUNC0_OUT_SEL_CFG) / 4;
        unsigned anterior = s->seleccion_salida[pin] & 0x1ff;

        s->seleccion_salida[pin] = v;
        esp32c3_ledc_actualizar_senal(anterior);
        esp32c3_ledc_actualizar_senal(v & 0x1ff);
        return;
    }
    uint64_t alto = (uint64_t)v << 32;

    switch (addr) {
    case A_ESP32S3_GPIO_OUT: s->out = (s->out & ~0xffffffffull) | v; break;
    case A_ESP32S3_GPIO_OUT_W1TS: s->out |= v; break;
    case A_ESP32S3_GPIO_OUT_W1TC: s->out &= ~(uint64_t)v; break;
    case A_ESP32S3_GPIO_OUT1: s->out = (s->out & 0xffffffffull) | alto; break;
    case A_ESP32S3_GPIO_OUT1_W1TS: s->out |= alto; break;
    case A_ESP32S3_GPIO_OUT1_W1TC: s->out &= ~alto; break;
    case A_ESP32S3_GPIO_ENABLE: s->enable = (s->enable & ~0xffffffffull) | v; break;
    case A_ESP32S3_GPIO_ENABLE_W1TS: s->enable |= v; break;
    case A_ESP32S3_GPIO_ENABLE_W1TC: s->enable &= ~(uint64_t)v; break;
    case A_ESP32S3_GPIO_ENABLE1: s->enable = (s->enable & 0xffffffffull) | alto; break;
    case A_ESP32S3_GPIO_ENABLE1_W1TS: s->enable |= alto; break;
    case A_ESP32S3_GPIO_ENABLE1_W1TC: s->enable &= ~alto; break;
    default: break;
    }
    s->out &= ESP32S3_GPIO_MASCARA;
    s->enable &= ESP32S3_GPIO_MASCARA;
    if (s->out != out_antes || s->enable != enable_antes) {
        taller_puente_gpio(s->out, s->enable);
        esp32s3_gpio_observar_armados(s);
    }
}

static const MemoryRegionOps esp32s3_gpio_ops = {
    .read = esp32s3_gpio_read,
    .write = esp32s3_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32s3_gpio_reset_hold(Object *obj, ResetType type)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);
    bool cambio = s->out || s->enable;

    s->out = s->enable = s->drenador_abierto = 0;
    s->in_externo = ESP32S3_GPIO_MASCARA;
    memset(s->registro_pin, 0, sizeof(s->registro_pin));
    for (unsigned pin = 0; pin < ESP32S3_GPIO_PINES; pin++) {
        s->seleccion_salida[pin] = ESP32S3_GPIO_SENAL_SOFTWARE;
    }
    for (unsigned senal = 73; senal <= 80; senal++) {
        esp32c3_ledc_actualizar_senal(senal);
    }
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_del(s->trenes[i].timer);
    }
    taller_armado_reiniciar(&s->armado, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (cambio) {
        taller_puente_gpio(0, 0);
    }
}

static void esp32s3_gpio_init(Object *obj)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);

    taller_armado_iniciar(&s->armado, 0);
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        s->trenes[i].gpio = s;
        s->trenes[i].indice = i;
        s->trenes[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        esp32s3_gpio_paso_armado, &s->trenes[i]);
    }
    object_property_set_int(obj, "strap_mode", ESP32S3_STRAP_MODE_FLASH_BOOT,
                            &error_fatal);
    memory_region_init_io(&s->iomem, obj, &esp32s3_gpio_ops, s,
                          TYPE_ESP32S3_GPIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void esp32s3_gpio_finalize(Object *obj)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_free(s->trenes[i].timer);
    }
}

static Property esp32s3_gpio_properties[] = {
    DEFINE_PROP_UINT32("strap_mode", ESP32S3GPIOState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_gpio_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_gpio_reset_hold;
    device_class_set_props(DEVICE_CLASS(klass), esp32s3_gpio_properties);
}

static const TypeInfo esp32s3_gpio_info = {
    .name = TYPE_ESP32S3_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3GPIOState),
    .instance_init = esp32s3_gpio_init,
    .instance_finalize = esp32s3_gpio_finalize,
    .class_init = esp32s3_gpio_class_init,
};

static void esp32s3_gpio_register_types(void)
{
    type_register_static(&esp32s3_gpio_info);
}

type_init(esp32s3_gpio_register_types)
