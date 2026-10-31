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
}

static const MemoryRegionOps uart_ops = {
    .read =  esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    s->out = 0;
    s->enable = 0;
    s->in_externo = ~0ull;
}

static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);

    memory_region_init_io(&s->iomem, obj, &uart_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
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
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
