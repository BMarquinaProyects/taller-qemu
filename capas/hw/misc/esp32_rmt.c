/*
 * RMT TX del ESP32 clásico. Cada símbolo se consume en tiempo virtual; la RAM
 * siguiente se consulta después, permitiendo la recarga ping-pong de ESP-IDF.
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/misc/esp32_rmt.h"
#include "hw/misc/taller_puente.h"
#include "hw/gpio/esp32_gpio.h"

#define RMT_SIG_OUT0_IDX 87

/* Diagnóstico: TALLER_RMT_TRAZA=1 vuelca a stderr los accesos a registros (no a la RAM). */
static bool esp32_rmt_traza(void)
{
    static int activa = -1;
    if (activa < 0) {
        activa = getenv("TALLER_RMT_TRAZA") != NULL;
    }
    return activa;
}

static void esp32_rmt_irq(Esp32RmtState *s)
{
    uint32_t st = taller_rmt_leer(&s->logica, 0xa4);
    if (esp32_rmt_traza()) {
        fprintf(stderr, "rmt IRQ %d (st %08x raw %08x)\n", st != 0, st, s->logica.registros[0xa0 / 4]);
    }
    qemu_set_irq(s->irq, st != 0);
}

static void esp32_rmt_programar(Esp32RmtCanal *c)
{
    TallerRmtTx *tx = &c->rmt->logica.tx[c->numero];
    c->pendiente = taller_rmt_cargar(&c->rmt->logica, c->numero);
    if (!c->pendiente.valido) {
        return;
    }
    if (taller_puente_activo()) {
        g_array_append_val(c->simbolos, c->pendiente.simbolo);
    }
    /* Plazos absolutos. APB puede tener ticks de 12,5 ns: redondear una sola
     * vez el acumulado, evitando añadir medio ns por pulso. */
    timer_mod_ns(c->timer, c->inicio_ns + c->desfase_ns + (tx->tiempo_ps + 999) / 1000);
}

/* La ISR limpió tx_thr_event (o desactivó esa IRQ): seguir donde se quedó. */
static void esp32_rmt_reanudar(Esp32RmtCanal *c)
{
    c->esperando_isr = false;
    c->desfase_ns += qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - c->pausa_desde_ns;
    if (c->rmt->logica.tx[c->numero].activo) {
        esp32_rmt_programar(c);
    }
}

static void esp32_rmt_timer(void *opaque)
{
    Esp32RmtCanal *c = opaque;
    Esp32RmtState *s = c->rmt;
    TallerRmtTx *tx = &s->logica.tx[c->numero];
    unsigned evento = taller_rmt_avanzar(&s->logica, c->numero, c->pendiente);

    if (evento & (TALLER_RMT_FIN | TALLER_RMT_VUELTA)) {
        taller_puente_rmt(c->numero, c->gpio, tx->tick_ps,
                          (const uint32_t *)c->simbolos->data,
                          c->simbolos->len, c->inicio_ns);
        g_array_set_size(c->simbolos, 0);
        if (evento & TALLER_RMT_VUELTA) {
            /* Una vuelta es un tren completo; continuar sin tx_end. */
            c->inicio_ns += c->desfase_ns + (tx->tiempo_ps + 999) / 1000;
            c->desfase_ns = 0;
            tx->tiempo_ps = 0;
        }
    }
    esp32_rmt_irq(s);
    /*
     * Contrapresión en el ping-pong. En el chip, la ISR del umbral entra en pocos µs y le
     * sobran los 40 µs que dura la otra mitad del bloque. En QEMU, si el firmware espera
     * en `waiti` (FreeRTOS bloqueado en rmtWrite), `-icount sleep=on` deja correr el reloj
     * virtual con el real y la ISR puede llegar tarde: el modelo leería la mitad sin
     * rellenar (colores al azar, distintos en cada ejecución). Así que, con la IRQ de
     * umbral activada, el canal se para hasta que la ISR la limpie en INT_CLR. Los
     * símbolos no cambian; solo se alarga un poco la duración virtual del tren.
     */
    if (tx->activo && (evento & TALLER_RMT_UMBRAL) &&
        (s->logica.registros[0xa8 / 4] & (1u << (24 + c->numero)))) {
        c->esperando_isr = true;
        c->pausa_desde_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    }
    if (tx->activo) {
        esp32_rmt_programar(c);
    }
}

static uint64_t esp32_rmt_read(void *opaque, hwaddr direccion, unsigned size)
{
    Esp32RmtState *s = opaque;
    uint64_t v = taller_rmt_leer(&s->logica, direccion);
    if (esp32_rmt_traza() && direccion < 0x800) {
        fprintf(stderr, "rmt L %03x = %08" PRIx64 "\n", (unsigned)direccion, v);
    }
    return v;
}

static void esp32_rmt_write(void *opaque, hwaddr direccion, uint64_t valor,
                            unsigned size)
{
    Esp32RmtState *s = opaque;
    if (esp32_rmt_traza() && direccion < 0x800) {
        fprintf(stderr, "rmt E %03x <- %08" PRIx64 "\n", (unsigned)direccion, valor);
    }
    unsigned iniciados = taller_rmt_escribir(&s->logica, direccion, valor);
    for (unsigned n = 0; n < TALLER_RMT_CANALES; n++) {
        if (iniciados & (1u << n)) {
            Esp32RmtCanal *c = &s->canal[n];
            int gpio = esp32_gpio_pin_de_senal(RMT_SIG_OUT0_IDX + n);
            timer_del(c->timer);
            g_array_set_size(c->simbolos, 0);
            c->inicio_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            c->desfase_ns = 0;
            c->esperando_isr = false;
            c->gpio = gpio < 0 ? 255 : gpio;
            esp32_rmt_programar(c);
        }
    }
    /* Reanudar los canales parados en el umbral: la ISR lo limpió o desactivó esa IRQ. */
    if (direccion == 0xac || direccion == 0xa8) {
        for (unsigned n = 0; n < TALLER_RMT_CANALES; n++) {
            Esp32RmtCanal *c = &s->canal[n];
            uint32_t bit = 1u << (24 + n);
            bool limpiado = direccion == 0xac && (valor & bit);
            bool desactivado = direccion == 0xa8 && !(valor & bit);
            if (c->esperando_isr && (limpiado || desactivado)) {
                esp32_rmt_reanudar(c);
            }
        }
    }
    esp32_rmt_irq(s);
}

static const MemoryRegionOps esp32_rmt_ops = {
    .read = esp32_rmt_read,
    .write = esp32_rmt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32_rmt_reset(Object *obj, ResetType tipo)
{
    Esp32RmtState *s = ESP32_RMT(obj);
    for (unsigned n = 0; n < TALLER_RMT_CANALES; n++) {
        timer_del(s->canal[n].timer);
        g_array_set_size(s->canal[n].simbolos, 0);
        s->canal[n].esperando_isr = false;
        s->canal[n].desfase_ns = 0;
    }
    taller_rmt_iniciar(&s->logica);
    esp32_rmt_irq(s);
}

static void esp32_rmt_init(Object *obj)
{
    Esp32RmtState *s = ESP32_RMT(obj);
    memory_region_init_io(&s->iomem, obj, &esp32_rmt_ops, s, TYPE_ESP32_RMT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    for (unsigned n = 0; n < TALLER_RMT_CANALES; n++) {
        Esp32RmtCanal *c = &s->canal[n];
        c->rmt = s;
        c->numero = n;
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp32_rmt_timer, c);
        c->simbolos = g_array_new(false, false, sizeof(uint32_t));
    }
    taller_rmt_iniciar(&s->logica);
}

static void esp32_rmt_finalize(Object *obj)
{
    Esp32RmtState *s = ESP32_RMT(obj);
    for (unsigned n = 0; n < TALLER_RMT_CANALES; n++) {
        timer_free(s->canal[n].timer);
        g_array_free(s->canal[n].simbolos, true);
    }
}

static void esp32_rmt_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = esp32_rmt_reset;
}

static const TypeInfo esp32_rmt_info = {
    .name = TYPE_ESP32_RMT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32RmtState),
    .instance_init = esp32_rmt_init,
    .instance_finalize = esp32_rmt_finalize,
    .class_init = esp32_rmt_class_init,
};

static void esp32_rmt_register_types(void)
{
    type_register_static(&esp32_rmt_info);
}

type_init(esp32_rmt_register_types)
