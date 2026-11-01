/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"
#include "hw/misc/esp32_ledc.h"
#include "hw/misc/taller_puente.h"



/* Taller: el GPIO del chip (uno por máquina), para que el puente pueda fijar entradas. */
static Esp32GpioState *gpio_del_chip;

int esp32_gpio_pin_de_senal(int senal)
{
    if (!gpio_del_chip || senal < 0 || senal > 511) {
        return -1;
    }
    for (int pin = 0; pin < ESP32_GPIO_CANTIDAD_PINES; pin++) {
        if (FIELD_EX32(gpio_del_chip->seleccion_salida[pin],
                       GPIO_FUNC0_OUT_SEL_CFG, OUT_SEL) == (uint32_t)senal) {
            return pin;
        }
    }
    return -1;
}

void esp32_gpio_fijar_entrada(uint8_t pin, bool nivel)
{
    if (!gpio_del_chip || pin > 39) {
        return;
    }
    if (nivel) {
        gpio_del_chip->in_externo |= 1ull << pin;
    } else {
        gpio_del_chip->in_externo &= ~(1ull << pin);
    }
}

void esp32_gpio_armar_entrada(const uint8_t *datos, size_t largo)
{
    if (gpio_del_chip) {
        taller_armado_recibir(&gpio_del_chip->armado, datos, largo);
    }
}

static void esp32_gpio_paso_armado(void *opaque)
{
    Esp32GpioTemporizadorArmado *t = opaque;
    TallerArmadoRanura *r = &t->gpio->armado.ranuras[t->indice];
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t pin;
    bool nivel;

    /* Incluye esperas cero y pasos vencidos, siempre con un máximo de 200. */
    while (taller_armado_avanzar(r, ahora_ns, &pin, &nivel)) {
        esp32_gpio_fijar_entrada(pin, nivel); /* misma ruta que ENTRADA */
    }
    if (r->sonando) {
        timer_mod_ns(t->timer, r->proximo_ns);
    }
}

static void esp32_gpio_observar_armados(Esp32GpioState *s)
{
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t disparos = taller_armado_observar(&s->armado, s->out, s->enable,
                                             ahora_ns);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        if (disparos & (1u << i)) {
            taller_puente_disparo(s->armado.ranuras[i].en_curso.pin_disparo,
                                  ahora_ns);
            /* Aplica el primer paso aquí si su espera es cero. */
            esp32_gpio_paso_armado(&s->trenes[i]);
        }
    }
}

/* Taller: los registros "1" (pines 32-39) usan solo 8 bits. */
static uint32_t parte_baja(uint64_t v) { return (uint32_t)v; }
static uint32_t parte_alta(uint64_t v) { return (uint32_t)(v >> 32) & ESP32_GPIO_MASCARA_ALTA; }
static uint64_t con_baja(uint64_t v, uint32_t baja) { return (v & ~0xFFFFFFFFull) | baja; }
static uint64_t con_alta(uint64_t v, uint32_t alta) {
    return (v & 0xFFFFFFFFull) | ((uint64_t)(alta & ESP32_GPIO_MASCARA_ALTA) << 32);
}

/* Lo que ve el firmware al leer IN: su propio nivel en las salidas, el externo en las entradas. */
static uint64_t esp32_gpio_entrada(Esp32GpioState *s)
{
    return (s->out & s->enable) | (s->in_externo & ~s->enable);
}

static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;
    if (addr >= A_GPIO_FUNC0_OUT_SEL_CFG &&
        addr < A_GPIO_FUNC0_OUT_SEL_CFG + 4 * ESP32_GPIO_CANTIDAD_PINES &&
        !(addr & 3)) {
        return s->seleccion_salida[(addr - A_GPIO_FUNC0_OUT_SEL_CFG) / 4];
    }
    switch (addr) {
    case A_GPIO_STRAP:
        r = s->strap_mode;
        break;
    case A_GPIO_OUT:     r = parte_baja(s->out); break;
    case A_GPIO_OUT1:    r = parte_alta(s->out); break;
    case A_GPIO_ENABLE:  r = parte_baja(s->enable); break;
    case A_GPIO_ENABLE1: r = parte_alta(s->enable); break;
    case A_GPIO_IN:      r = parte_baja(esp32_gpio_entrada(s)); break;
    case A_GPIO_IN1:     r = parte_alta(esp32_gpio_entrada(s)); break;
    default:
        break;
    }
    return r;
}

static void esp32_gpio_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint32_t v = (uint32_t)value;
    uint64_t out_antes = s->out, enable_antes = s->enable;
    if (addr >= A_GPIO_FUNC0_OUT_SEL_CFG &&
        addr < A_GPIO_FUNC0_OUT_SEL_CFG + 4 * ESP32_GPIO_CANTIDAD_PINES &&
        !(addr & 3)) {
        int pin = (int)((addr - A_GPIO_FUNC0_OUT_SEL_CFG) / 4);
        int anterior = FIELD_EX32(s->seleccion_salida[pin],
                                 GPIO_FUNC0_OUT_SEL_CFG, OUT_SEL);
        int nueva = FIELD_EX32(v, GPIO_FUNC0_OUT_SEL_CFG, OUT_SEL);

        /* Conservar también inversión y habilitación para las lecturas del SDK. */
        s->seleccion_salida[pin] = v;
        if (anterior != nueva) {
            /* La señal anterior puede perder su pin; la nueva puede ganarlo. */
            esp32_ledc_actualizar_senal(anterior);
            esp32_ledc_actualizar_senal(nueva);
        }
        return;
    }
    switch (addr) {
    case A_GPIO_OUT:          s->out = con_baja(s->out, v); break;
    case A_GPIO_OUT_W1TS:     s->out |= v; break;
    case A_GPIO_OUT_W1TC:     s->out &= ~(uint64_t)v; break;
    case A_GPIO_OUT1:         s->out = con_alta(s->out, v); break;
    case A_GPIO_OUT1_W1TS:    s->out |= (uint64_t)(v & ESP32_GPIO_MASCARA_ALTA) << 32; break;
    case A_GPIO_OUT1_W1TC:    s->out &= ~((uint64_t)(v & ESP32_GPIO_MASCARA_ALTA) << 32); break;
    case A_GPIO_ENABLE:       s->enable = con_baja(s->enable, v); break;
    case A_GPIO_ENABLE_W1TS:  s->enable |= v; break;
    case A_GPIO_ENABLE_W1TC:  s->enable &= ~(uint64_t)v; break;
    case A_GPIO_ENABLE1:      s->enable = con_alta(s->enable, v); break;
    case A_GPIO_ENABLE1_W1TS: s->enable |= (uint64_t)(v & ESP32_GPIO_MASCARA_ALTA) << 32; break;
    case A_GPIO_ENABLE1_W1TC: s->enable &= ~((uint64_t)(v & ESP32_GPIO_MASCARA_ALTA) << 32); break;
    default:
        break;
    }
    if (s->out != out_antes || s->enable != enable_antes) {
        taller_puente_gpio(s->out, s->enable);
        esp32_gpio_observar_armados(s);
    }
}

static const MemoryRegionOps uart_ops = {
    .read =  esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    uint32_t anteriores[ESP32_GPIO_CANTIDAD_PINES];

    memcpy(anteriores, s->seleccion_salida, sizeof(anteriores));
    s->out = 0;
    s->enable = 0;
    s->in_externo = ~0ull;
    /* Reset del chip: conservar sensores armados, cancelar trenes en curso. */
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_del(s->trenes[i].timer);
    }
    taller_armado_reiniciar(&s->armado, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    for (int pin = 0; pin < ESP32_GPIO_CANTIDAD_PINES; pin++) {
        s->seleccion_salida[pin] = ESP32_GPIO_SENAL_SOFTWARE;
    }
    /* Recalcular después de desconectar toda la matriz evita rutas intermedias. */
    for (int pin = 0; pin < ESP32_GPIO_CANTIDAD_PINES; pin++) {
        esp32_ledc_actualizar_senal(FIELD_EX32(anteriores[pin],
                                            GPIO_FUNC0_OUT_SEL_CFG, OUT_SEL));
    }
}

static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    gpio_del_chip = s;
    /* Todos altos y antigüedad desde t=0, antes de cualquier acceso GPIO. */
    taller_armado_iniciar(&s->armado, 0);
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        s->trenes[i].gpio = s;
        s->trenes[i].indice = i;
        s->trenes[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         esp32_gpio_paso_armado, &s->trenes[i]);
    }

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);

    memory_region_init_io(&s->iomem, obj, &uart_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void esp32_gpio_finalize(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_del(s->trenes[i].timer);
        timer_free(s->trenes[i].timer);
    }
    if (gpio_del_chip == s) {
        gpio_del_chip = NULL;
    }
}

static Property esp32_gpio_properties[] = {
    /* The strap_mode needs to be explicitly set in the instance init, thus, set
     * the default value to 0. */
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_gpio_reset_hold;
    dc->realize = esp32_gpio_realize;
    device_class_set_props(dc, esp32_gpio_properties);
}

static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .instance_finalize = esp32_gpio_finalize,
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
