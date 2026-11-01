/* RMT TX C3: dos canales, RAM de 48 símbolos por bloque y recarga por IRQ.
 * Registros contrastados con rmt_reg.h/rmt_ll.h de Arduino 3.3.12 (IDF 5.5).
 * GPL-2.0-or-later. Sin RX, DMA, portadora ni transmisión continua.
 */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/misc/esp32c3_rmt.h"
#include "hw/gpio/esp32c3_gpio.h"
#include "hw/misc/taller_puente.h"

/* Estado y registros independientes del RMT clásico. */
#define C3_RMT_CONF 0x10
#define C3_RMT_RAW 0x38
#define C3_RMT_ST 0x3c
#define C3_RMT_ENA 0x40
#define C3_RMT_CLR 0x44
#define C3_RMT_LIM 0x58
#define C3_RMT_SYS 0x68
#define C3_RMT_PULSOS 0x01800087u

static unsigned esp32c3_rmt_capacidad(ESP32C3RMTState *s, unsigned canal)
{
    unsigned bloques = (s->registros[(C3_RMT_CONF + 4 * canal) / 4] >> 16) & 7;

    /* Los bloques TX se prestan entre sí; los dos bloques RX no son TX. */
    return MIN(bloques, ESP32C3_RMT_CANALES_TX - canal) *
           ESP32C3_RMT_PALABRAS_BLOQUE;
}

static uint32_t esp32c3_rmt_tick_ps(ESP32C3RMTState *s, unsigned canal)
{
    uint32_t sys = s->registros[C3_RMT_SYS / 4];
    unsigned fuente = (sys >> 24) & 3;
    uint32_t hz = fuente == 1 ? 80000000 :
                  fuente == 2 ? 17500000 : fuente == 3 ? 40000000 : 0;
    unsigned divisor = (s->registros[(C3_RMT_CONF + 4 * canal) / 4] >> 8) & 255;
    unsigned entero = ((sys >> 4) & 255) + 1;
    unsigned numerador = (sys >> 12) & 63;
    unsigned denominador = (sys >> 18) & 63;

    if (!hz || !(sys & (1u << 26))) {
        return 0;
    }
    if (!denominador) {
        denominador = 1;
        numerador = 0;
    }
    /* rmt_sclk = fuente / (1 + NUM + A/B), después divisor del canal.
     * Redondear solo el tick final, manteniendo los 12,5 ns de APB. */
    uint64_t ps = 1000000000000ULL * (divisor ? divisor : 256) *
                  (entero * denominador + numerador);
    uint64_t base = (uint64_t)hz * denominador;

    return (uint32_t)((ps + base / 2) / base);
}

static void esp32c3_rmt_irq(ESP32C3RMTState *s)
{
    s->registros[C3_RMT_ST / 4] = s->registros[C3_RMT_RAW / 4] &
                                 s->registros[C3_RMT_ENA / 4];
    qemu_set_irq(s->irq, s->registros[C3_RMT_ST / 4] != 0);
}

static void esp32c3_rmt_programar(ESP32C3RMTCanal *c)
{
    ESP32C3RMTState *s = c->rmt;
    uint32_t simbolo = s->memoria[c->numero * ESP32C3_RMT_PALABRAS_BLOQUE +
                                 c->posicion];
    unsigned d0 = simbolo & 0x7fff;
    unsigned d1 = (simbolo >> 16) & 0x7fff;

    /* Consultar solo el símbolo que empieza ahora. La ISR aún puede cambiar
     * cualquier símbolo futuro, incluso dentro de la misma mitad de RAM. */
    c->ticks_pendientes = d0 ? d0 + d1 : 0;
    c->fin_pendiente = !d0 || !d1;
    c->tiempo_ps += (uint64_t)c->ticks_pendientes * c->tick_ps;
    if (taller_puente_activo()) {
        g_array_append_val(c->simbolos, simbolo);
    }
    timer_mod_ns(c->timer, c->inicio_ns + c->desfase_ns +
                 (c->tiempo_ps + 999) / 1000);
}

static void esp32c3_rmt_terminar(ESP32C3RMTCanal *c)
{
    c->activo = false;
    c->esperando_isr = false;
    c->rmt->registros[C3_RMT_RAW / 4] |= 1u << c->numero;
    taller_puente_rmt((uint8_t)c->numero, c->gpio, c->tick_ps,
                     (const uint32_t *)c->simbolos->data,
                     c->simbolos->len, c->inicio_ns);
    g_array_set_size(c->simbolos, 0);
}

static void esp32c3_rmt_timer(void *opaque)
{
    ESP32C3RMTCanal *c = opaque;
    ESP32C3RMTState *s = c->rmt;
    uint32_t conf = s->registros[(C3_RMT_CONF + 4 * c->numero) / 4];
    unsigned limite = s->registros[(C3_RMT_LIM + 4 * c->numero) / 4] & 511;
    unsigned capacidad = esp32c3_rmt_capacidad(s, c->numero);
    uint32_t umbral = 1u << (8 + c->numero);
    bool evento_umbral = false;

    if (!c->activo) {
        return;
    }
    if (c->ticks_pendientes) {
        c->posicion++;
        if (limite && ++c->desde_umbral >= limite) {
            c->desde_umbral = 0;
            s->registros[C3_RMT_RAW / 4] |= umbral;
            evento_umbral = true;
        }
    }
    if (c->fin_pendiente || !capacidad ||
        (c->posicion >= capacidad && !(conf & (1u << 4)))) {
        esp32c3_rmt_terminar(c);
    } else if (c->posicion >= capacidad) {
        c->posicion = 0;
    }
    esp32c3_rmt_irq(s);
    if (!c->activo) {
        return;
    }
    /* Como en el RMT clásico, dar contrapresión cuando QEMU duerme: no
     * consumir otra mitad hasta atender la IRQ. INT_CLR puede preceder al
     * encoder: este rellena la mitad recién enviada; la siguiente mitad ya
     * estaba preparada por la ISR anterior (o antes de TX_START). */
    if (evento_umbral && (s->registros[C3_RMT_ENA / 4] & umbral)) {
        c->esperando_isr = true;
        c->pausa_desde_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    }
    esp32c3_rmt_programar(c);
}

static bool esp32c3_rmt_registro_valido(hwaddr addr)
{
    return !(addr & 3) && (addr <= 0x70 || addr == 0xcc);
}

static uint64_t esp32c3_rmt_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3RMTState *s = ESP32C3_RMT(opaque);

    if (addr & 3) {
        return 0;
    }
    if (addr >= ESP32C3_RMT_RAM &&
        addr < ESP32C3_RMT_RAM + sizeof(s->memoria)) {
        return (s->registros[C3_RMT_SYS / 4] & 1) ?
               s->memoria[(addr - ESP32C3_RMT_RAM) / 4] : 0;
    }
    if (!esp32c3_rmt_registro_valido(addr)) {
        return 0;
    }
    if (addr < 0x10) {
        unsigned canal = (unsigned)addr / 4;
        unsigned p = s->apb_lectura[canal];
        unsigned capacidad = canal < 2 ? esp32c3_rmt_capacidad(s, canal) : 48;

        if ((s->registros[C3_RMT_SYS / 4] & 1) || p >= capacidad) {
            return 0;
        }
        s->apb_lectura[canal]++;
        return s->memoria[canal * 48 + p];
    }
    if (addr == 0x28 || addr == 0x2c) {
        unsigned canal = ((unsigned)addr - 0x28) / 4;
        ESP32C3RMTCanal *c = &s->canal[canal];

        return (canal * 48 + c->posicion) | (c->activo ? 1u << 9 : 0) |
               (s->apb_escritura[canal] << 12);
    }
    if (addr == C3_RMT_CLR || addr == 0x70) {
        return 0;
    }
    return s->registros[addr / 4];
}

static void esp32c3_rmt_iniciar_canal(ESP32C3RMTCanal *c)
{
    ESP32C3RMTState *s = c->rmt;
    unsigned capacidad = esp32c3_rmt_capacidad(s, c->numero);
    int gpio = esp32c3_gpio_pin_de_senal(s->gpio, ESP32C3_RMT_SENAL0 + c->numero);

    timer_del(c->timer);
    g_array_set_size(c->simbolos, 0);
    c->inicio_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->desfase_ns = 0;
    c->esperando_isr = false;
    c->desde_umbral = 0;
    c->tiempo_ps = 0;
    c->tick_ps = esp32c3_rmt_tick_ps(s, c->numero);
    c->gpio = gpio < 0 ? 255 : (uint8_t)gpio;
    c->activo = capacidad && c->posicion < capacidad && c->tick_ps;
    if (c->activo) {
        esp32c3_rmt_programar(c);
    } else {
        s->registros[C3_RMT_RAW / 4] |= 1u << (4 + c->numero);
    }
}

static void esp32c3_rmt_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    ESP32C3RMTState *s = ESP32C3_RMT(opaque);
    uint32_t v = (uint32_t)value;

    if (addr & 3) {
        return;
    }
    if (addr >= ESP32C3_RMT_RAM &&
        addr < ESP32C3_RMT_RAM + sizeof(s->memoria)) {
        if (s->registros[C3_RMT_SYS / 4] & 1) {
            s->memoria[(addr - ESP32C3_RMT_RAM) / 4] = v;
        }
        return;
    }
    if (!esp32c3_rmt_registro_valido(addr)) {
        return;
    }
    if (addr < 0x10) {
        unsigned canal = (unsigned)addr / 4;
        unsigned p = s->apb_escritura[canal];
        unsigned capacidad = canal < 2 ? esp32c3_rmt_capacidad(s, canal) : 48;

        if (!(s->registros[C3_RMT_SYS / 4] & 1) && p < capacidad) {
            s->memoria[canal * 48 + p] = v;
            s->apb_escritura[canal]++;
        }
        return;
    }
    if ((addr >= 0x28 && addr <= C3_RMT_ST) || addr == 0xcc) {
        return; /* Estado, IRQ raw y fecha: solo lectura. */
    }
    if (addr == C3_RMT_CLR) {
        s->registros[C3_RMT_RAW / 4] &= ~v;
    } else if (addr == C3_RMT_CONF || addr == C3_RMT_CONF + 4) {
        unsigned canal = ((unsigned)addr - C3_RMT_CONF) / 4;
        ESP32C3RMTCanal *c = &s->canal[canal];

        s->registros[addr / 4] = v & ~C3_RMT_PULSOS;
        if (v & (1u << 1)) {
            c->posicion = c->desde_umbral = 0;
        }
        if (v & ((1u << 2) | (1u << 23))) {
            s->apb_lectura[canal] = s->apb_escritura[canal] = 0;
        }
        if (v & (1u << 7)) {
            timer_del(c->timer);
            c->activo = c->esperando_isr = false;
            g_array_set_size(c->simbolos, 0);
        } else if (v & 1) {
            esp32c3_rmt_iniciar_canal(c);
        }
    } else if (addr == 0x70) {
        return; /* REF_CNT_RST: pulso, no reinicia el puntero de RAM. */
    } else {
        if (addr == C3_RMT_LIM || addr == C3_RMT_LIM + 4) {
            v &= ~(1u << 20); /* LOOP_COUNT_RESET, sin modo continuo. */
        }
        s->registros[addr / 4] = v;
    }
    if (addr == C3_RMT_CLR || addr == C3_RMT_ENA) {
        for (unsigned n = 0; n < ESP32C3_RMT_CANALES_TX; n++) {
            ESP32C3RMTCanal *c = &s->canal[n];
            uint32_t bit = 1u << (8 + n);
            bool limpiar = addr == C3_RMT_CLR && (v & bit);
            bool desactivar = addr == C3_RMT_ENA && !(v & bit);

            if (c->esperando_isr && (limpiar || desactivar)) {
                c->esperando_isr = false;
                c->desfase_ns += qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                                 c->pausa_desde_ns;
                esp32c3_rmt_programar(c);
            }
        }
    }
    esp32c3_rmt_irq(s);
}

static const MemoryRegionOps esp32c3_rmt_ops = {
    .read = esp32c3_rmt_read,
    .write = esp32c3_rmt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_rmt_reiniciar(Object *obj, ResetType tipo)
{
    ESP32C3RMTState *s = ESP32C3_RMT(obj);

    memset(s->registros, 0, sizeof(s->registros));
    memset(s->memoria, 0, sizeof(s->memoria));
    memset(s->apb_lectura, 0, sizeof(s->apb_lectura));
    memset(s->apb_escritura, 0, sizeof(s->apb_escritura));
    s->registros[C3_RMT_SYS / 4] = 0x05000010;
    s->registros[0xcc / 4] = 0x02006231;
    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_TX; n++) {
        ESP32C3RMTCanal *c = &s->canal[n];

        timer_del(c->timer);
        g_array_set_size(c->simbolos, 0);
        c->posicion = c->desde_umbral = c->tick_ps = c->ticks_pendientes = 0;
        c->tiempo_ps = 0;
        c->activo = c->esperando_isr = c->fin_pendiente = false;
        c->inicio_ns = c->pausa_desde_ns = c->desfase_ns = 0;
        c->gpio = 255;
        s->registros[(C3_RMT_CONF + 4 * n) / 4] = 0x00710200;
        s->registros[(C3_RMT_LIM + 4 * n) / 4] = 128;
    }
    esp32c3_rmt_irq(s);
}

static void esp32c3_rmt_init(Object *obj)
{
    ESP32C3RMTState *s = ESP32C3_RMT(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_rmt_ops, s,
                          TYPE_ESP32C3_RMT, ESP32C3_RMT_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_TX; n++) {
        ESP32C3RMTCanal *c = &s->canal[n];

        c->rmt = s;
        c->numero = n;
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp32c3_rmt_timer, c);
        c->simbolos = g_array_new(false, false, sizeof(uint32_t));
    }
    esp32c3_rmt_reiniciar(obj, RESET_TYPE_COLD);
}

static void esp32c3_rmt_finalize(Object *obj)
{
    ESP32C3RMTState *s = ESP32C3_RMT(obj);

    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_TX; n++) {
        timer_free(s->canal[n].timer);
        g_array_free(s->canal[n].simbolos, true);
    }
}

static void esp32c3_rmt_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_rmt_reiniciar;
}

static const TypeInfo esp32c3_rmt_info = {
    .name = TYPE_ESP32C3_RMT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3RMTState),
    .instance_init = esp32c3_rmt_init,
    .instance_finalize = esp32c3_rmt_finalize,
    .class_init = esp32c3_rmt_class_init,
};

static void esp32c3_rmt_register_types(void)
{
    type_register_static(&esp32c3_rmt_info);
}

type_init(esp32c3_rmt_register_types)
