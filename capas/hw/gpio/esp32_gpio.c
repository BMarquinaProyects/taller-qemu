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
#include "hw/gpio/taller_pull.h"
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

void esp32_gpio_fijar_entrada(uint8_t pin, uint8_t nivel)
{
    Esp32GpioState *s = gpio_del_chip;

    if (!s || pin > 39) {
        return;
    }
    uint64_t bit = 1ull << pin;
    if (nivel == 2) {
        s->sueltos |= bit;
        s->in_externo = taller_pull_resolver(s->in_externo, s->sueltos,
                                             s->pull_arriba, s->pull_abajo);
    } else {
        s->sueltos &= ~bit;
        if (nivel) {
            s->in_externo |= bit;
        } else {
            s->in_externo &= ~bit;
        }
    }
}

static void esp32_gpio_pull_actualizar(Esp32GpioState *s)
{
    uint64_t arriba, abajo;

    taller_pull_mascaras(s->registros_iomux,
                         s->registros_rtcio, ESP32_GPIO_CANTIDAD_PINES,
                         0xffffffffffull, &arriba, &abajo);
    if (s->pull_arriba != arriba || s->pull_abajo != abajo) {
        s->pull_arriba = arriba;
        s->pull_abajo = abajo;
        s->in_externo = taller_pull_resolver(s->in_externo, s->sueltos,
                                             arriba, abajo);
        taller_puente_pull(arriba, abajo);
    }
}

void esp32_gpio_armar_entrada(const uint8_t *datos, size_t largo)
{
    if (gpio_del_chip && largo >= 9 && datos[0] < 40 && datos[6] < 40) {
        taller_armado_recibir(&gpio_del_chip->armado, datos, largo);
    }
}

/* ENTRADA_TREN (v8): cada paso aplica su nivel por la misma ruta que ENTRADA. */
static void esp32_gpio_paso_tren(void *opaque)
{
    Esp32GpioTemporizadorArmado *t = opaque;
    TallerTrenRanura *r = &t->gpio->tren.ranuras[t->indice];
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t pin, nivel;

    while (taller_tren_avanzar(r, ahora_ns, &pin, &nivel)) {
        esp32_gpio_fijar_entrada(pin, nivel);
    }
    if (r->activa) {
        timer_mod_ns(t->timer, r->proximo_ns);
    }
}

void esp32_gpio_tren_entrada(const uint8_t *datos, size_t largo)
{
    Esp32GpioState *s = gpio_del_chip;
    int cancelada, ranura;

    if (!s) {
        return;
    }
    ranura = taller_tren_recibir(&s->tren, datos, largo, 0xffffffffffull,
                                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                 &cancelada);
    if (cancelada >= 0) {
        timer_del(s->temporizadores_tren[cancelada].timer);
    }
    if (ranura >= 0) {
        timer_del(s->temporizadores_tren[ranura].timer);
        esp32_gpio_paso_tren(&s->temporizadores_tren[ranura]);
    }
}

void esp32_gpio_cancelar_tren(uint8_t pin)
{
    Esp32GpioState *s = gpio_del_chip;
    int ranura = s ? taller_tren_cancelar(&s->tren, pin) : -1;

    if (ranura >= 0) {
        timer_del(s->temporizadores_tren[ranura].timer);
    }
}

/* UNHILO (v9): los sensores 1-Wire contestan aquí mismo, con el reloj virtual (M-244 de Taller). */
void esp32_gpio_unhilo(const uint8_t *datos, size_t largo)
{
    if (gpio_del_chip) {
        taller_unhilo_recibir(&gpio_del_chip->unhilo, datos, largo, 0xffffffffffull);
    }
}

static void esp32_gpio_cuanto_armado(void *opaque);

static void esp32_gpio_observar_unhilo(Esp32GpioState *s)
{
    TallerUnHiloPulso pulsos[TALLER_UNHILO_BUSES];
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned n = taller_unhilo_observar(&s->unhilo, s->out, s->enable, ahora_ns, pulsos);
    int64_t cola_ns = taller_unhilo_cola_ns(&s->unhilo);

    /* Con el bus en marcha, el mismo cuanto corto que los trenes armados (M-157). */
    if (cola_ns > s->armado.fin_cola_ns) {
        s->armado.fin_cola_ns = cola_ns;
    }
    if (taller_armado_cuanto_activo(&s->armado, ahora_ns) && !timer_pending(s->cuanto_armado)) {
        esp32_gpio_cuanto_armado(s);
    }

    for (unsigned i = 0; i < n; i++) {
        uint8_t carga[13];
        esp32_gpio_tren_entrada(carga, taller_unhilo_tren(&pulsos[i], carga));
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
        if (!r->sonando) {
            t->gpio->armado.fin_cola_ns = taller_armado_plazo(
                ahora_ns, TALLER_ARMADO_CUANTO_COLA_NS);
        }
    }
    if (r->sonando) {
        timer_mod_ns(t->timer, r->proximo_ns);
    }
}

/* M-157: acotar turnos durante preparación, trenes y cola del último flanco.
 * Si el tick coincide con el último paso, antes ve el tren y deja +1 us
 * pendiente; después ve la cola. Entre ticks ya hay uno pendiente.
 * Diagnóstico confirmado: docs/m157-armado.md, sección Conclusión. */
static void esp32_gpio_cuanto_armado(void *opaque)
{
    Esp32GpioState *s = opaque;
    int64_t ahora_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (taller_armado_cuanto_activo(&s->armado, ahora_ns)) {
        timer_mod_ns(s->cuanto_armado,
                     taller_armado_plazo(ahora_ns, TALLER_ARMADO_CUANTO_NS));
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
    if (taller_armado_cuanto_activo(&s->armado, ahora_ns) &&
        !timer_pending(s->cuanto_armado)) {
        esp32_gpio_cuanto_armado(s);
    }
}

/* Taller: los registros "1" (pines 32-39) usan solo 8 bits. */
static uint32_t parte_baja(uint64_t v) { return (uint32_t)v; }
static uint32_t parte_alta(uint64_t v) { return (uint32_t)(v >> 32) & ESP32_GPIO_MASCARA_ALTA; }
static uint64_t con_baja(uint64_t v, uint32_t baja) { return (v & ~0xFFFFFFFFull) | baja; }
static uint64_t con_alta(uint64_t v, uint32_t alta) {
    return (v & 0xFFFFFFFFull) | ((uint64_t)(alta & ESP32_GPIO_MASCARA_ALTA) << 32);
}

/*
 * Lo que ve el firmware al leer IN: su propio nivel en las salidas push-pull, el externo en las
 * entradas y, en las salidas de drenador abierto (PAD_DRIVER), el AND cableado de las dos: un 1
 * suelta la línea y manda lo de fuera (el DHT22 de MicroPython la usa así, E-037).
 */
static uint64_t esp32_gpio_entrada(Esp32GpioState *s)
{
    uint64_t od = s->enable & s->drenador_abierto;
    return (s->out & s->enable & ~od) | (s->out & s->in_externo & od) | (s->in_externo & ~s->enable);
}

/* GPIO_PINn_REG (0x88 + 4n): se guarda entero; el bit 2 (PAD_DRIVER) es el drenador abierto. */
#define A_GPIO_PIN0 0x0088
#define ESP32_GPIO_PAD_DRIVER (1u << 2)

static bool es_registro_pin(hwaddr addr)
{
    return addr >= A_GPIO_PIN0 && addr < A_GPIO_PIN0 + 4 * ESP32_GPIO_CANTIDAD_PINES && !(addr & 3);
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
    if (es_registro_pin(addr)) {
        return s->registro_pin[(addr - A_GPIO_PIN0) / 4];
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
    if (es_registro_pin(addr)) {
        int pin = (int)((addr - A_GPIO_PIN0) / 4);
        s->registro_pin[pin] = v;
        if (v & ESP32_GPIO_PAD_DRIVER) {
            s->drenador_abierto |= 1ull << pin;
        } else {
            s->drenador_abierto &= ~(1ull << pin);
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
        esp32_gpio_observar_unhilo(s);
    }
}

static const MemoryRegionOps uart_ops = {
    .read =  esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t esp32_iomux_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);

    return s->registros_iomux[addr / 4];
}

static void esp32_iomux_write(void *opaque, hwaddr addr, uint64_t valor,
                              unsigned size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);

    s->registros_iomux[addr / 4] = valor;
    esp32_gpio_pull_actualizar(s);
}

static const MemoryRegionOps esp32_iomux_ops = {
    .read = esp32_iomux_read,
    .write = esp32_iomux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t esp32_rtcio_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);

    return s->registros_rtcio[addr / 4];
}

static void esp32_rtcio_write(void *opaque, hwaddr addr, uint64_t valor,
                              unsigned size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);

    s->registros_rtcio[addr / 4] = valor;
    esp32_gpio_pull_actualizar(s);
}

static const MemoryRegionOps esp32_rtcio_ops = {
    .read = esp32_rtcio_read,
    .write = esp32_rtcio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    uint32_t anteriores[ESP32_GPIO_CANTIDAD_PINES];

    memcpy(anteriores, s->seleccion_salida, sizeof(anteriores));
    s->out = 0;
    s->enable = 0;
    s->drenador_abierto = 0;
    memset(s->registro_pin, 0, sizeof(s->registro_pin));
    /* El cable y la carga de las entradas sueltas sobreviven al reset. */
    s->in_externo |= 0xffffffffffull & ~s->sueltos;
    memset(s->registros_iomux, 0, sizeof(s->registros_iomux));
    memset(s->registros_rtcio, 0, sizeof(s->registros_rtcio));
    esp32_gpio_pull_actualizar(s);
    /* Reset del chip: conservar sensores armados, cancelar trenes en curso. */
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_del(s->trenes[i].timer);
    }
    timer_del(s->cuanto_armado);
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        timer_del(s->temporizadores_tren[i].timer);
    }
    taller_tren_reiniciar(&s->tren);
    taller_unhilo_reiniciar(&s->unhilo);
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
    s->cuanto_armado = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   esp32_gpio_cuanto_armado, s);
    taller_tren_iniciar(&s->tren);
    taller_unhilo_iniciar(&s->unhilo);
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        s->temporizadores_tren[i].gpio = s;
        s->temporizadores_tren[i].indice = i;
        s->temporizadores_tren[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                       esp32_gpio_paso_tren,
                                                       &s->temporizadores_tren[i]);
    }

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);

    memory_region_init_io(&s->iomem, obj, &uart_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomux, obj, &esp32_iomux_ops, s,
                          "esp32.iomux", TALLER_IOMUX_BYTES);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomux);
    memory_region_init_io(&s->rtcio, obj, &esp32_rtcio_ops, s,
                          "esp32.rtcio", TALLER_RTCIO_BYTES);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->rtcio);
}

static void esp32_gpio_finalize(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        timer_del(s->trenes[i].timer);
        timer_free(s->trenes[i].timer);
    }
    timer_del(s->cuanto_armado);
    timer_free(s->cuanto_armado);
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        timer_del(s->temporizadores_tren[i].timer);
        timer_free(s->temporizadores_tren[i].timer);
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
