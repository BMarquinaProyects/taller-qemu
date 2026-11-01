/*
 * ESP32-C3 GPIO emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 Taller.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 *
 * Taller, fase 6d: el original solo heredaba el GPIO clásico (STRAP).
 * Aquí hay 22 GPIO, OUT/ENABLE/IN, W1TS/W1TC, PAD_DRIVER y trenes armados.
 * PINn y FUNCn_OUT_SEL_CFG conservan lo escrito en sus offsets C3; LEDC usa
 * la selección de salida. No hay inversión, IRQ GPIO ni filtros.
 * IO_MUX sigue sin modelo en la máquina: lecturas cero/escrituras ignoradas;
 * IN usa el nivel externo, inicialmente alto, como en el clásico. No se
 * simulan resistencias ni INPUT_PULLDOWN. STRAP es una propiedad independiente.
 *
 * Auditoría de la máquina base esp-develop-9.2.2-20260417:
 * - Dos UART (modelo esp32c3_uart sobre esp32_uart), FIFO, timeout e IRQ.
 * - Dos TIMG (un T0 por grupo y WDT) y SYSTIMER (dos contadores/tres alarmas).
 * - intmatrix ya guarda irq_levels, hace OR por línea y recalcula ambas
 *   líneas al reasignar, además de prioridad/umbral/enable. No tiene el fallo
 *   del clásico; no emula selección de IRQ por flanco.
 * - SPI1 sirve la flash; caché, eFuse y GDMA ya existen. LEDC y ADC1 tienen
 *   modelos C3 propios. I2C externo, RMT y SPI2 de usuario siguen pendientes.
 *   Según soc_caps.h del core 3.3.12: LEDC tiene 4 timers/6 canales LS;
 *   ADC1, GPIO0..4; I2C, un controlador; RMT, 2 TX/2 RX de 48 palabras.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32c3_gpio.h"
#include "hw/misc/taller_puente.h"
#include "hw/misc/esp32c3_ledc.h"

void esp32c3_gpio_fijar_entrada(ESP32C3GPIOState *s, uint8_t pin, bool nivel)
{
    if (pin >= ESP32C3_GPIO_PINES) {
        return;
    }
    if (nivel) {
        s->in_externo |= 1u << pin;
    } else {
        s->in_externo &= ~(1u << pin);
    }
}

void esp32c3_gpio_armar_entrada(ESP32C3GPIOState *s, const uint8_t *datos,
                               size_t largo)
{
    /* Validar antes de sustituir: taller_armado admite también los 40 clásicos. */
    if (largo >= 9 && datos[0] < ESP32C3_GPIO_PINES &&
        datos[6] < ESP32C3_GPIO_PINES) {
        taller_armado_recibir(&s->armado, datos, largo);
    }
}

int esp32c3_gpio_pin_de_senal(ESP32C3GPIOState *s, int senal)
{
    if (s && senal >= 0 && senal < ESP32C3_GPIO_SENAL_SOFTWARE) {
        for (unsigned pin = 0; pin < ESP32C3_GPIO_PINES; pin++) {
            if ((s->seleccion_salida[pin] & 0xff) == (unsigned)senal) {
                return pin; /* Como el clásico: primera ruta si hay varias. */
            }
        }
    }
    return -1;
}

static void esp32c3_gpio_paso_armado(void *opaque)
{
    ESP32C3GpioTren *t = opaque;
    TallerArmadoRanura *r = &t->gpio->armado.ranuras[t->indice];
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t pin;
    bool nivel;

    while (taller_armado_avanzar(r, ahora_ns, &pin, &nivel)) {
        esp32c3_gpio_fijar_entrada(t->gpio, pin, nivel);
    }
    if (r->sonando) {
        timer_mod_ns(t->timer, r->proximo_ns);
    }
}

static void esp32c3_gpio_observar_armados(ESP32C3GPIOState *s)
{
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t disparos = taller_armado_observar(&s->armado, s->out, s->enable,
                                             ahora_ns);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        if (disparos & (1u << i)) {
            taller_puente_disparo(s->armado.ranuras[i].en_curso.pin_disparo,
                                  ahora_ns);
            esp32c3_gpio_paso_armado(&s->trenes[i]);
        }
    }
}

static uint32_t esp32c3_gpio_entrada(ESP32C3GPIOState *s)
{
    uint32_t od = s->enable & s->drenador_abierto;

    return ((s->out & s->enable & ~od) | (s->out & s->in_externo & od) |
            (s->in_externo & ~s->enable)) & ESP32C3_GPIO_MASCARA;
}

static bool esp32c3_gpio_en_tabla(hwaddr addr, hwaddr base)
{
    return addr >= base && addr < base + 4 * ESP32C3_GPIO_PINES &&
           !(addr & 3);
}

static uint64_t esp32c3_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);

    if (esp32c3_gpio_en_tabla(addr, A_ESP32C3_GPIO_PIN0)) {
        return s->registro_pin[(addr - A_ESP32C3_GPIO_PIN0) / 4];
    }
    if (esp32c3_gpio_en_tabla(addr, A_ESP32C3_GPIO_FUNC0_OUT_SEL_CFG)) {
        return s->seleccion_salida[(addr - A_ESP32C3_GPIO_FUNC0_OUT_SEL_CFG) / 4];
    }
    switch (addr) {
    case A_ESP32C3_GPIO_OUT: return s->out;
    case A_ESP32C3_GPIO_ENABLE: return s->enable;
    case A_ESP32C3_GPIO_IN: return esp32c3_gpio_entrada(s);
    case A_ESP32C3_GPIO_STRAP: return s->strap_mode;
    default: return 0;
    }
}

static void esp32c3_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);
    uint32_t v = value;
    uint32_t out_antes = s->out, enable_antes = s->enable;

    if (esp32c3_gpio_en_tabla(addr, A_ESP32C3_GPIO_PIN0)) {
        unsigned pin = (addr - A_ESP32C3_GPIO_PIN0) / 4;

        s->registro_pin[pin] = v;
        if (v & ESP32C3_GPIO_PAD_DRIVER) {
            s->drenador_abierto |= 1u << pin;
        } else {
            s->drenador_abierto &= ~(1u << pin);
        }
        return;
    }
    if (esp32c3_gpio_en_tabla(addr, A_ESP32C3_GPIO_FUNC0_OUT_SEL_CFG)) {
        unsigned pin = (addr - A_ESP32C3_GPIO_FUNC0_OUT_SEL_CFG) / 4;
        unsigned anterior = s->seleccion_salida[pin] & 0xff;

        s->seleccion_salida[pin] = v;
        esp32c3_ledc_actualizar_senal(anterior);
        esp32c3_ledc_actualizar_senal(v & 0xff);
        return;
    }
    v &= ESP32C3_GPIO_MASCARA;
    switch (addr) {
    case A_ESP32C3_GPIO_OUT: s->out = v; break;
    case A_ESP32C3_GPIO_OUT_W1TS: s->out |= v; break;
    case A_ESP32C3_GPIO_OUT_W1TC: s->out &= ~v; break;
    case A_ESP32C3_GPIO_ENABLE: s->enable = v; break;
    case A_ESP32C3_GPIO_ENABLE_W1TS: s->enable |= v; break;
    case A_ESP32C3_GPIO_ENABLE_W1TC: s->enable &= ~v; break;
    default: break;
    }
    if (s->out != out_antes || s->enable != enable_antes) {
        taller_puente_gpio(s->out, s->enable);
        esp32c3_gpio_observar_armados(s);
    }
}

static const MemoryRegionOps esp32c3_gpio_ops = {
    .read = esp32c3_gpio_read,
    .write = esp32c3_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_gpio_reset_hold(Object *obj, ResetType type)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    bool cambio = s->out || s->enable;

    s->out = s->enable = s->drenador_abierto = 0;
    s->in_externo = ESP32C3_GPIO_MASCARA;
    memset(s->registro_pin, 0, sizeof(s->registro_pin));
    for (unsigned pin = 0; pin < ESP32C3_GPIO_PINES; pin++) {
        s->seleccion_salida[pin] = ESP32C3_GPIO_SENAL_SOFTWARE;
    }
    for (unsigned senal = 45; senal <= 50; senal++) {
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

static void esp32c3_gpio_init(Object *obj)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);

    taller_armado_iniciar(&s->armado, 0);
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        s->trenes[i].gpio = s;
        s->trenes[i].indice = i;
        s->trenes[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        esp32c3_gpio_paso_armado, &s->trenes[i]);
    }
    object_property_set_int(obj, "strap_mode", ESP32C3_STRAP_MODE_FLASH_BOOT,
                            &error_fatal);
    memory_region_init_io(&s->iomem, obj, &esp32c3_gpio_ops, s,
                          TYPE_ESP32C3_GPIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void esp32c3_gpio_finalize(Object *obj)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_free(s->trenes[i].timer);
    }
}

static Property esp32c3_gpio_properties[] = {
    DEFINE_PROP_UINT32("strap_mode", ESP32C3GPIOState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_gpio_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_gpio_reset_hold;
    device_class_set_props(DEVICE_CLASS(klass), esp32c3_gpio_properties);
}

static const TypeInfo esp32c3_gpio_info = {
    .name = TYPE_ESP32C3_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3GPIOState),
    .instance_init = esp32c3_gpio_init,
    .instance_finalize = esp32c3_gpio_finalize,
    .class_init = esp32c3_gpio_class_init,
};

static void esp32c3_gpio_register_types(void)
{
    type_register_static(&esp32c3_gpio_info);
}

type_init(esp32c3_gpio_register_types)
