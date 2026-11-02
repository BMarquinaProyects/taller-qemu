/* RMT TX C3/S3: dos/cuatro TX, bloques de 48 símbolos y recarga por IRQ.
 * Registros contrastados con rmt_reg.h/rmt_ll.h de Arduino 3.3.12 (IDF 5.5).
 * GPL-2.0-or-later. Sin RX, DMA, portadora ni transmisión continua.
 */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/misc/esp32c3_rmt.h"
#include "hw/qdev-properties.h"
#include "hw/misc/taller_puente.h"

/* Estado y registros independientes del RMT clásico.
 * S3 conserva los bits TX y de reloj, pero desplaza grupos al duplicar canales.
 * La máquina selecciona el mapa antes de realize; el C3 es el predeterminado.
 */
typedef struct ESP32C3RMTMapa {
    unsigned canales_tx, senal0;
    unsigned conf, estado_tx, raw, st, ena, clr, lim, sys, ref_rst, ultimo;
    unsigned ram;
    unsigned mem_size_mascara, estado_apb;
    uint32_t estado_activo, fecha;
} ESP32C3RMTMapa;

static const ESP32C3RMTMapa mapa_c3 = {
    .canales_tx = 2, .senal0 = 51,
    .conf = 0x10, .estado_tx = 0x28,
    .raw = 0x38, .st = 0x3c, .ena = 0x40, .clr = 0x44,
    .lim = 0x58, .sys = 0x68, .ref_rst = 0x70, .ultimo = 0x70,
    .ram = 0x400,
    .mem_size_mascara = 7, .estado_apb = 12,
    .estado_activo = 1u << 9, .fecha = 0x02006231,
};
static const ESP32C3RMTMapa mapa_s3 = {
    .canales_tx = 4, .senal0 = 81,
    .conf = 0x20, .estado_tx = 0x50,
    .raw = 0x70, .st = 0x74, .ena = 0x78, .clr = 0x7c,
    .lim = 0xa0, .sys = 0xc0, .ref_rst = 0xc8, .ultimo = 0xcc,
    .ram = 0x800,
    .mem_size_mascara = 15, .estado_apb = 11,
    .estado_activo = 1u << 22, .fecha = 0x02101181,
};
#define C3_RMT_PULSOS 0x01800087u

static const ESP32C3RMTMapa *esp32c3_rmt_mapa(ESP32C3RMTState *s)
{
    return s->s3 ? &mapa_s3 : &mapa_c3;
}

static unsigned esp32c3_rmt_capacidad(ESP32C3RMTState *s, unsigned canal)
{
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);
    unsigned bloques = (s->registros[(m->conf + 4 * canal) / 4] >> 16) &
                       m->mem_size_mascara;

    /* Solo se prestan bloques TX; los bloques RX nunca amplían un TX. */
    return MIN(bloques, m->canales_tx - canal) *
           ESP32C3_RMT_PALABRAS_BLOQUE;
}

static uint32_t esp32c3_rmt_tick_ps(ESP32C3RMTState *s, unsigned canal)
{
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);
    uint32_t sys = s->registros[m->sys / 4];
    unsigned fuente = (sys >> 24) & 3;
    uint32_t hz = fuente == 1 ? 80000000 :
                  fuente == 2 ? 17500000 : fuente == 3 ? 40000000 : 0;
    unsigned divisor = (s->registros[(m->conf + 4 * canal) / 4] >> 8) & 255;
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
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);

    s->registros[m->st / 4] = s->registros[m->raw / 4] &
                             s->registros[m->ena / 4];
    qemu_set_irq(s->irq, s->registros[m->st / 4] != 0);
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
    ESP32C3RMTState *s = c->rmt;
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);

    c->activo = false;
    c->esperando_isr = false;
    c->rmt->registros[m->raw / 4] |= 1u << c->numero;
    taller_puente_rmt((uint8_t)c->numero, c->gpio, c->tick_ps,
                     (const uint32_t *)c->simbolos->data,
                     c->simbolos->len, c->inicio_ns);
    g_array_set_size(c->simbolos, 0);
}

static void esp32c3_rmt_timer(void *opaque)
{
    ESP32C3RMTCanal *c = opaque;
    ESP32C3RMTState *s = c->rmt;
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);
    uint32_t conf = s->registros[(m->conf + 4 * c->numero) / 4];
    unsigned limite = s->registros[(m->lim + 4 * c->numero) / 4] & 511;
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
            s->registros[m->raw / 4] |= umbral;
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
    if (evento_umbral && (s->registros[m->ena / 4] & umbral)) {
        c->esperando_isr = true;
        c->pausa_desde_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    }
    esp32c3_rmt_programar(c);
}

static bool esp32c3_rmt_registro_valido(ESP32C3RMTState *s, hwaddr addr)
{
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);

    return !(addr & 3) && (addr <= m->ultimo || addr == 0xcc);
}

static uint64_t esp32c3_rmt_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3RMTState *s = ESP32C3_RMT(opaque);
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);

    if (addr & 3) {
        return 0;
    }
    if (addr >= m->ram && addr < m->ram + m->canales_tx * 2 * 48 * 4) {
        return (s->registros[m->sys / 4] & 1) ?
               s->memoria[(addr - m->ram) / 4] : 0;
    }
    if (!esp32c3_rmt_registro_valido(s, addr)) {
        return 0;
    }
    if (addr < m->conf) {
        unsigned canal = (unsigned)addr / 4;
        unsigned p = s->apb_lectura[canal];
        unsigned capacidad = canal < m->canales_tx ? esp32c3_rmt_capacidad(s, canal) : 48;

        if ((s->registros[m->sys / 4] & 1) || p >= capacidad) {
            return 0;
        }
        s->apb_lectura[canal]++;
        return s->memoria[canal * 48 + p];
    }
    if (addr >= m->estado_tx && addr < m->estado_tx + 4 * m->canales_tx) {
        unsigned canal = ((unsigned)addr - m->estado_tx) / 4;
        ESP32C3RMTCanal *c = &s->canal[canal];

        return (canal * 48 + c->posicion) | (c->activo ? m->estado_activo : 0) |
               (s->apb_escritura[canal] << m->estado_apb);
    }
    if (addr == m->clr || addr == m->ref_rst) {
        return 0;
    }
    return s->registros[addr / 4];
}

static void esp32c3_rmt_iniciar_canal(ESP32C3RMTCanal *c)
{
    ESP32C3RMTState *s = c->rmt;
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);
    unsigned capacidad = esp32c3_rmt_capacidad(s, c->numero);
    int gpio = s->pin_de_senal ?
               s->pin_de_senal(s->gpio, m->senal0 + c->numero) : -1;

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
        s->registros[m->raw / 4] |= 1u << (4 + c->numero);
    }
}

static void esp32c3_rmt_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    ESP32C3RMTState *s = ESP32C3_RMT(opaque);
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);
    uint32_t v = (uint32_t)value;

    if (addr & 3) {
        return;
    }
    if (addr >= m->ram && addr < m->ram + m->canales_tx * 2 * 48 * 4) {
        if (s->registros[m->sys / 4] & 1) {
            s->memoria[(addr - m->ram) / 4] = v;
        }
        return;
    }
    if (!esp32c3_rmt_registro_valido(s, addr)) {
        return;
    }
    if (addr < m->conf) {
        unsigned canal = (unsigned)addr / 4;
        unsigned p = s->apb_escritura[canal];
        unsigned capacidad = canal < m->canales_tx ? esp32c3_rmt_capacidad(s, canal) : 48;

        if (!(s->registros[m->sys / 4] & 1) && p < capacidad) {
            s->memoria[canal * 48 + p] = v;
            s->apb_escritura[canal]++;
        }
        return;
    }
    if ((addr >= m->estado_tx && addr <= m->st) || addr == 0xcc) {
        return; /* Estado, IRQ raw y fecha: solo lectura. */
    }
    if (addr == m->clr) {
        s->registros[m->raw / 4] &= ~v;
    } else if (addr >= m->conf && addr < m->conf + 4 * m->canales_tx) {
        unsigned canal = ((unsigned)addr - m->conf) / 4;
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
    } else if (addr == m->ref_rst) {
        return; /* REF_CNT_RST: pulso, no reinicia el puntero de RAM. */
    } else {
        if (addr >= m->lim && addr < m->lim + 4 * m->canales_tx) {
            v &= ~(1u << 20); /* LOOP_COUNT_RESET, sin modo continuo. */
        }
        s->registros[addr / 4] = v;
    }
    if (addr == m->clr || addr == m->ena) {
        for (unsigned n = 0; n < m->canales_tx; n++) {
            ESP32C3RMTCanal *c = &s->canal[n];
            uint32_t bit = 1u << (8 + n);
            bool limpiar = addr == m->clr && (v & bit);
            bool desactivar = addr == m->ena && !(v & bit);

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
    const ESP32C3RMTMapa *m = esp32c3_rmt_mapa(s);

    memset(s->registros, 0, sizeof(s->registros));
    memset(s->memoria, 0, sizeof(s->memoria));
    memset(s->apb_lectura, 0, sizeof(s->apb_lectura));
    memset(s->apb_escritura, 0, sizeof(s->apb_escritura));
    s->registros[m->sys / 4] = 0x05000010;
    s->registros[0xcc / 4] = m->fecha;
    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_MAX; n++) {
        ESP32C3RMTCanal *c = &s->canal[n];

        timer_del(c->timer);
        g_array_set_size(c->simbolos, 0);
        c->posicion = c->desde_umbral = c->tick_ps = c->ticks_pendientes = 0;
        c->tiempo_ps = 0;
        c->activo = c->esperando_isr = c->fin_pendiente = false;
        c->inicio_ns = c->pausa_desde_ns = c->desfase_ns = 0;
        c->gpio = 255;
        if (n < m->canales_tx) {
            s->registros[(m->conf + 4 * n) / 4] = 0x00710200;
            s->registros[(m->lim + 4 * n) / 4] = 128;
        }
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
    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_MAX; n++) {
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

    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_MAX; n++) {
        timer_free(s->canal[n].timer);
        g_array_free(s->canal[n].simbolos, true);
    }
}

static void esp32c3_rmt_realize(DeviceState *dev, Error **errp)
{
    esp32c3_rmt_reiniciar(OBJECT(dev), RESET_TYPE_COLD);
}

static Property esp32c3_rmt_properties[] = {
    DEFINE_PROP_BOOL("s3", ESP32C3RMTState, s3, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_rmt_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_rmt_reiniciar;
    DEVICE_CLASS(klass)->realize = esp32c3_rmt_realize;
    device_class_set_props(DEVICE_CLASS(klass), esp32c3_rmt_properties);
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
