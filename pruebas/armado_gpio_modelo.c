/* GPIO clásico y S3 reales con reloj y temporizadores dobles, sin QEMU. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_armado.h"
#include "hw/gpio/taller_pull.h"

typedef int SysBusDevice;
typedef int SysBusDeviceClass;
typedef int MemoryRegion;
typedef int qemu_irq;
typedef void Object;
typedef void DeviceState;
typedef int ResetType;
typedef int Error;
typedef uint64_t hwaddr;
typedef struct QEMUTimer {
    int64_t plazo;
    bool pendiente;
    bool liberado;
    void (*callback)(void *);
    void *opaque;
} QEMUTimer;
typedef struct MemoryRegionOps {
    uint64_t (*read)(void *, hwaddr, unsigned);
    void (*write)(void *, hwaddr, uint64_t, unsigned);
    int endianness;
    struct { unsigned min_access_size, max_access_size; } valid, impl;
} MemoryRegionOps;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
#define SYS_BUS_DEVICE(obj) ((SysBusDevice *)(obj))
#define DEVICE_LITTLE_ENDIAN 0
#define REG32(nombre, addr) enum { A_##nombre = addr };
#define FIELD(reg, nombre, pos, ancho)
#define FIELD_EX32(valor, reg, nombre) ((valor) & 0x1ff)
#define QEMU_CLOCK_VIRTUAL 1
static int64_t ahora;
static unsigned cantidad_disparos;
static QEMUTimer timers[TALLER_ARMADO_RANURAS + 1];
static unsigned cantidad_timers;
static Error *error_fatal;

static int64_t qemu_clock_get_ns(int reloj)
{
    assert(reloj == QEMU_CLOCK_VIRTUAL);
    return ahora;
}
static QEMUTimer *timer_new_ns(int reloj, void (*callback)(void *), void *opaque)
{
    assert(reloj == QEMU_CLOCK_VIRTUAL && cantidad_timers < 9);
    QEMUTimer *t = &timers[cantidad_timers++];
    *t = (QEMUTimer){.callback = callback, .opaque = opaque};
    return t;
}
static void timer_mod_ns(QEMUTimer *t, int64_t plazo)
{
    assert(t && !t->liberado);
    t->plazo = plazo;
    t->pendiente = true;
}
static bool timer_pending(const QEMUTimer *t) { return t->pendiente; }
static void timer_del(QEMUTimer *t) { t->pendiente = false; }
static void timer_free(QEMUTimer *t)
{
    assert(!t->liberado);
    t->pendiente = false;
    t->liberado = true;
}
static void atender(QEMUTimer *t, int64_t t_ns)
{
    assert(t->pendiente && t_ns >= t->plazo);
    ahora = t_ns;
    t->pendiente = false; /* QEMU lo retira de la lista antes del callback. */
    t->callback(t->opaque);
}
#ifdef MODELO_S3
static void esp32c3_ledc_actualizar_senal(int senal) { (void)senal; }
#else
static void esp32_ledc_actualizar_senal(int senal) { (void)senal; }
#endif
static void object_property_set_int(Object *obj, const char *nombre,
                                    int valor, Error **errp) {}
static void memory_region_init_io(MemoryRegion *mr, Object *obj,
                                  const MemoryRegionOps *ops, void *opaque,
                                  const char *nombre, uint64_t largo) {}
static void sysbus_init_mmio(SysBusDevice *s, MemoryRegion *mr) {}
#ifndef MODELO_S3
static void sysbus_init_irq(SysBusDevice *s, qemu_irq *irq) {}
#endif
static void taller_puente_gpio(uint64_t out, uint64_t enable)
{
    (void)out;
    (void)enable;
}
static void taller_puente_disparo(uint8_t pin, int64_t t_ns)
{
    assert((pin == 4 || pin == 5) && t_ns == ahora);
    cantidad_disparos++;
}

static void taller_puente_pull(uint64_t arriba, uint64_t abajo) {}

/* CABECERA_REAL */
/* MODELO_REAL */

#ifdef MODELO_S3
typedef ESP32S3GPIOState Esp32GpioState;
#define esp32_gpio_init esp32s3_gpio_init
#define esp32_gpio_reset_hold esp32s3_gpio_reset_hold
#define esp32_gpio_finalize esp32s3_gpio_finalize
#define esp32_gpio_write esp32s3_gpio_write
#define esp32_gpio_read esp32s3_gpio_read
#define A_GPIO_ENABLE_W1TS A_ESP32S3_GPIO_ENABLE_W1TS
#define A_GPIO_ENABLE_W1TC A_ESP32S3_GPIO_ENABLE_W1TC
#define A_GPIO_OUT_W1TS A_ESP32S3_GPIO_OUT_W1TS
#define A_GPIO_OUT_W1TC A_ESP32S3_GPIO_OUT_W1TC
#define A_GPIO_IN A_ESP32S3_GPIO_IN
#define armar(gpio, carga, largo) esp32s3_gpio_armar_entrada(gpio, carga, largo)
#else
#define armar(gpio, carga, largo) esp32_gpio_armar_entrada(carga, largo)
#endif

static void iniciar(Esp32GpioState *gpio)
{
    memset(timers, 0, sizeof(timers));
    cantidad_timers = 0;
    ahora = 0;
#ifndef MODELO_S3
    (void)esp32_gpio_realize;
#endif
    esp32_gpio_init(gpio);
    esp32_gpio_reset_hold(gpio, 0);
}

static void finalizar(Esp32GpioState *gpio)
{
    esp32_gpio_finalize(gpio);
    for (unsigned i = 0; i < cantidad_timers; i++) {
        assert(timers[i].liberado && !timers[i].pendiente);
    }
}

static QEMUTimer *buscar_cuanto(const Esp32GpioState *gpio)
{
    for (unsigned i = 0; i < cantidad_timers; i++) {
        bool es_tren = false;
        for (unsigned j = 0; j < TALLER_ARMADO_RANURAS; j++) {
            es_tren |= &timers[i] == gpio->trenes[j].timer;
        }
        if (!es_tren) {
            return &timers[i];
        }
    }
    return NULL;
}

static void comprobar_tren(unsigned paso_tardio)
{
    static const uint8_t carga[] = {
        4, 1, 0x00, 0x35, 0x0c, 0, 4, 6, 0,
        0x30, 0x75, 0, 0, 0,
        0x80, 0x38, 1, 0, 1,
        0x80, 0x38, 1, 0, 0,
        0x50, 0xc3, 0, 0, 1,
        0x70, 0x11, 1, 0, 0,
        0x50, 0xc3, 0, 0, 1
    };
    static const int64_t previstos[] = {
        1030000, 1110000, 1190000, 1240000, 1310000, 1360000
    };
    static const bool niveles[] = {false, true, false, true, false, true};
    Esp32GpioState gpio = {0};
    int64_t flancos[6];

    iniciar(&gpio);
    /* La ruta MMIO real suelta GPIO4 tras 1 ms bajo y programa el tren. */
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 4, 4);
    armar(&gpio, carga, sizeof(carga));
    ahora = 1000000;
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    assert(timers[0].plazo == 1030000);
    for (unsigned i = 0; i < 6; i++) {
        assert(timers[0].plazo == previstos[i]);
        ahora = previstos[i] + (i == paso_tardio ? 4000 : 0);
        atender(gpio.trenes[0].timer, ahora);
        assert(!!(esp32_gpio_read(&gpio, A_GPIO_IN, 4) & (1u << 4)) == niveles[i]);
        flancos[i] = ahora;
    }
    assert(!gpio.armado.ranuras[0].sonando);
    /* Caracteriza la atención tardía; no afirma reproducir su causa en CI. */
    if (paso_tardio == 0) {
        assert(flancos[1] - flancos[0] == 76000);
        assert(flancos[4] - flancos[3] == 70000);
    } else {
        assert(flancos[1] - flancos[0] == 80000);
        assert(flancos[4] - flancos[3] == 74000);
        assert(flancos[5] - flancos[4] == 46000);
    }
    finalizar(&gpio);
}

static void cuanto_y_solapamiento(void)
{
    uint8_t carga[] = {4, 1, 0, 0, 0, 0, 18, 2, 0,
                      0x30, 0x75, 0, 0, 0, /* 30 us -> 0 */
                      0x80, 0x38, 1, 0, 1}; /* 80 us -> 1 */
    Esp32GpioState gpio = {0};
    iniciar(&gpio);
    QEMUTimer *cuanto = buscar_cuanto(&gpio);
    assert(cuanto != NULL);
    assert(!timer_pending(cuanto));
    armar(&gpio, carga, sizeof(carga));
    carga[0] = 5;
    carga[6] = 19;
    armar(&gpio, carga, sizeof(carga));
    assert(!timer_pending(cuanto)); /* Armar no significa estar sonando. */
    ahora = INT64_C(5000000000); /* Más allá del long de 32 bits (E-045). */
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, (1u << 4) | (1u << 5), 4);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    assert(cuanto->pendiente && cuanto->plazo == INT64_C(5000001000));
    assert(gpio.trenes[0].timer->plazo == INT64_C(5000030000));
    atender(cuanto, INT64_C(5000001000));
    assert(cuanto->pendiente && cuanto->plazo == INT64_C(5000002000));
    assert(esp32_gpio_read(&gpio, A_GPIO_IN, 4) & (1u << 18));
    ahora = INT64_C(5000001500);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 5, 4);
    assert(cuanto->plazo == INT64_C(5000002000)); /* No posponer el pendiente. */
    assert(gpio.trenes[1].timer->plazo == INT64_C(5000031500));
    atender(cuanto, INT64_C(5000002300)); /* Rearme desde el ahora del callback. */
    assert(cuanto->plazo == INT64_C(5000003300));
    atender(gpio.trenes[0].timer, INT64_C(5000030000));
    atender(gpio.trenes[1].timer, INT64_C(5000031500));
    atender(gpio.trenes[0].timer, INT64_C(5000110000));
    assert(!gpio.armado.ranuras[0].sonando && gpio.armado.ranuras[1].sonando);
    atender(cuanto, INT64_C(5000110000));
    assert(cuanto->pendiente && cuanto->plazo == INT64_C(5000111000));
    atender(gpio.trenes[1].timer, INT64_C(5000111500));
    atender(cuanto, INT64_C(5000111500));
    assert(cuanto->pendiente && cuanto->plazo == INT64_C(5000112500));
    for (int64_t t_ns = INT64_C(5000112500);
         t_ns < INT64_C(5000131500); t_ns += 1000) {
        atender(cuanto, t_ns);
        assert(cuanto->pendiente && cuanto->plazo == t_ns + 1000);
    }
    atender(cuanto, INT64_C(5000131500));
    assert(!timer_pending(cuanto));

    /* El siguiente disparo vuelve a arrancarlo; reset cancela todos los timers. */
    ahora = INT64_C(5000200000);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, (1u << 4) | (1u << 5), 4);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, (1u << 4) | (1u << 5), 4);
    assert(cuanto->pendiente && cuanto->plazo == INT64_C(5000201000));
    esp32_gpio_reset_hold(&gpio, 0);
    for (unsigned i = 0; i < cantidad_timers; i++) {
        assert(!timers[i].pendiente);
    }
    assert(gpio.armado.ranuras[0].armada && gpio.armado.ranuras[1].armada);

    /* Incluso un tren de espera cero deja la cola pendiente. */
    carga[0] = 4;
    carga[7] = 1;
    memset(carga + 9, 0, 5);
    armar(&gpio, carga, 14);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 4, 4);
    ahora++;
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    assert(!gpio.armado.ranuras[0].sonando && timer_pending(cuanto));
    assert(cuanto->plazo == ahora + 999); /* Conserva el tick de preparación. */
    finalizar(&gpio);
}

static void cola_y_orden_de_callbacks(unsigned orden)
{
    uint8_t carga[] = {4, 1, 0, 0, 0, 0, 18, 1, 0,
                      0x88, 0x13, 0, 0, 0}; /* 5 us -> 0 */
    Esp32GpioState gpio = {0};
    int64_t ultimo_ns = INT64_C(5000005000);
    if (orden == 2) {
        carga[9] = 0x7c;
        carga[10] = 0x15; /* 5,5 us: último paso entre ticks. */
        ultimo_ns = INT64_C(5000005500);
    }
    iniciar(&gpio);
    QEMUTimer *cuanto = buscar_cuanto(&gpio);
    armar(&gpio, carga, sizeof(carga));
    ahora = INT64_C(5000000000);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 4, 4);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    for (int64_t t_ns = INT64_C(5000001000);
         t_ns < INT64_C(5000005000); t_ns += 1000) {
        atender(cuanto, t_ns);
    }
    if (orden != 1) {
        atender(cuanto, INT64_C(5000005000)); /* Tick antes del paso. */
    }
    atender(gpio.trenes[0].timer, ultimo_ns);
    assert(!gpio.armado.ranuras[0].sonando);
    assert(!(esp32_gpio_read(&gpio, A_GPIO_IN, 4) & (1u << 18)));
    /* Ya hay plazo <= 1 us antes de calcular el próximo presupuesto. */
    assert(cuanto->pendiente && cuanto->plazo >= ahora);
    assert(cuanto->plazo <= ahora + 1000);
    if (orden == 1) {
        atender(cuanto, ultimo_ns); /* Paso antes del tick coincidente. */
    }
    assert(cuanto->plazo == INT64_C(5000006000));
    int64_t fin_ns = ultimo_ns + 20000;
    while (cuanto->plazo < fin_ns) {
        int64_t t_ns = cuanto->plazo;
        atender(cuanto, t_ns);
        assert(cuanto->pendiente && cuanto->plazo == t_ns + 1000);
    }
    atender(cuanto, cuanto->plazo);
    assert(!timer_pending(cuanto));
    finalizar(&gpio);
}

static void tren_dentro_de_cola(bool reiniciar)
{
    uint8_t carga[] = {4, 1, 0, 0, 0, 0, 18, 1, 0,
                      0x88, 0x13, 0, 0, 0}; /* 5 us -> 0 */
    Esp32GpioState gpio = {0};
    iniciar(&gpio);
    QEMUTimer *cuanto = buscar_cuanto(&gpio);
    armar(&gpio, carga, sizeof(carga));
    ahora = INT64_C(5000000000);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 4, 4);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    for (int64_t t_ns = INT64_C(5000001000);
         t_ns <= INT64_C(5000010000); t_ns += 1000) {
        if (t_ns == INT64_C(5000005000)) {
            atender(gpio.trenes[0].timer, t_ns);
        }
        atender(cuanto, t_ns);
    }
    /* Nuevo tren a +10 us, antes del fin de cola original (+25 us). */
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 4, 4);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    assert(cuanto->plazo == INT64_C(5000011000));
    for (int64_t t_ns = INT64_C(5000011000);
         t_ns <= INT64_C(5000025000); t_ns += 1000) {
        if (t_ns == INT64_C(5000015000)) {
            atender(gpio.trenes[0].timer, t_ns);
        }
        atender(cuanto, t_ns);
        assert(cuanto->pendiente && cuanto->plazo == t_ns + 1000);
    }
    assert(!gpio.armado.ranuras[0].sonando);
    if (reiniciar) {
        /* El reset dentro de la cola prolongada impide su resurrección. */
        esp32_gpio_reset_hold(&gpio, 0);
        for (unsigned i = 0; i < cantidad_timers; i++) {
            assert(!timers[i].pendiente);
        }
        cuanto->callback(cuanto->opaque);
    } else {
        for (int64_t t_ns = INT64_C(5000026000);
             t_ns < INT64_C(5000035000); t_ns += 1000) {
            atender(cuanto, t_ns);
            assert(cuanto->pendiente && cuanto->plazo == t_ns + 1000);
        }
        atender(cuanto, INT64_C(5000035000));
    }
    assert(!timer_pending(cuanto));
    assert(gpio.armado.ranuras[0].armada);
    finalizar(&gpio);
}

static void conducir(Esp32GpioState *gpio, bool alto)
{
    esp32_gpio_write(gpio, alto ? A_GPIO_OUT_W1TS : A_GPIO_OUT_W1TC,
                      1u << 5, 4);
}

static void preparacion_y_limite(bool subida)
{
    uint8_t carga[] = {5, 0, 0x40, 0x1f, 0, 0, 19, 1, 0,
                       0x30, 0x75, 0, 0, 0};
    const int64_t base_ns = INT64_C(5000000000);
    Esp32GpioState gpio = {0};
    iniciar(&gpio);
    QEMUTimer *cuanto = buscar_cuanto(&gpio);
    ahora = base_ns;
    conducir(&gpio, subida);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 5, 4);
    assert(!timer_pending(cuanto)); /* Un pin no armado no prepara nada. */
    carga[1] = subida;
    armar(&gpio, carga, sizeof(carga));
    ahora = base_ns + 1000;
    conducir(&gpio, !subida);
    assert(!gpio.armado.ranuras[0].sonando);
    assert(cuanto->pendiente && cuanto->plazo == base_ns + 2000);
    for (int64_t t_ns = base_ns + 2000; t_ns < base_ns + 29000; t_ns += 1000) {
        atender(cuanto, t_ns);
        assert(cuanto->pendiente && cuanto->plazo == t_ns + 1000);
    }
    atender(cuanto, base_ns + 29000); /* 8 us + 20 us desde el cambio. */
    assert(!timer_pending(cuanto));
    /* Soltar un pin alto mantiene el pull-up: no reinicia su edad. */
    if (!subida) {
        esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 5, 4);
        assert(!timer_pending(cuanto));
    }
    esp32_gpio_reset_hold(&gpio, 0);
    assert(!timer_pending(cuanto) && !gpio.trenes[0].timer->pendiente);
    finalizar(&gpio);
}

static void pulso_corto_y_enlace(bool subida)
{
    uint8_t carga[] = {5, 0, 0x40, 0x1f, 0, 0, 19, 1, 0,
                       0x30, 0x75, 0, 0, 0};
    const int64_t base_ns = INT64_C(5000000000);
    Esp32GpioState gpio = {0};
    unsigned antes = cantidad_disparos;
    iniciar(&gpio);
    QEMUTimer *cuanto = buscar_cuanto(&gpio);
    ahora = base_ns;
    conducir(&gpio, subida);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 5, 4);
    carga[1] = subida;
    armar(&gpio, carga, sizeof(carga));
    conducir(&gpio, !subida);
    assert(cuanto->pendiente && cuanto->plazo == base_ns + 1000);
    for (int64_t t_ns = base_ns + 1000; t_ns < base_ns + 5000; t_ns += 1000) {
        atender(cuanto, t_ns);
    }
    ahora = base_ns + 5000;
    conducir(&gpio, subida); /* 5 us: no alcanza el mínimo de 8 us. */
    assert(cantidad_disparos == antes && !gpio.trenes[0].timer->pendiente);
    assert(!taller_armado_cuanto_activo(&gpio.armado, ahora));
    atender(cuanto, ahora);
    assert(!timer_pending(cuanto));

    ahora = base_ns + 10000;
    conducir(&gpio, !subida);
    for (int64_t t_ns = base_ns + 11000; t_ns < base_ns + 18000; t_ns += 1000) {
        atender(cuanto, t_ns);
    }
    ahora = base_ns + 18000;
    conducir(&gpio, subida); /* Disparo al mínimo inclusivo. */
    assert(cantidad_disparos == antes + 1 && gpio.armado.ranuras[0].sonando);
    assert(cuanto->pendiente && cuanto->plazo == ahora); /* Sin aplazar el tick. */
    atender(cuanto, ahora);
    assert(cuanto->pendiente && cuanto->plazo == base_ns + 19000);
    atender(gpio.trenes[0].timer, base_ns + 48000);
    /* La cola conserva actividad aunque otro pulso corto ya haya terminado. */
    conducir(&gpio, !subida);
    ahora = base_ns + 53000;
    conducir(&gpio, subida);
    assert(cantidad_disparos == antes + 1);
    atender(cuanto, ahora);
    assert(cuanto->pendiente && cuanto->plazo == base_ns + 54000);
    atender(cuanto, base_ns + 68000);
    assert(!timer_pending(cuanto));

    ahora = base_ns + 100000;
    conducir(&gpio, !subida);
    assert(timer_pending(cuanto));
    esp32_gpio_reset_hold(&gpio, 0);
    assert(!timer_pending(cuanto) && !gpio.trenes[0].timer->pendiente);
    assert(gpio.armado.ranuras[0].armada);
    /* Una escritura posterior al reset vuelve a iniciar preparación. */
    conducir(&gpio, subida);
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 5, 4);
    conducir(&gpio, !subida);
    assert(cuanto->pendiente && cuanto->plazo == ahora + 1000);
    carga[7] = 0;
    armar(&gpio, carga, 9);
    atender(cuanto, cuanto->plazo);
    assert(!timer_pending(cuanto));
    finalizar(&gpio);
}

int main(void)
{
    comprobar_tren(0);
    comprobar_tren(4);
    assert(cantidad_disparos == 2);
    cuanto_y_solapamiento();
    assert(cantidad_disparos == 7);
    for (unsigned orden = 0; orden < 3; orden++) {
        cola_y_orden_de_callbacks(orden);
    }
    tren_dentro_de_cola(false);
    tren_dentro_de_cola(true);
    assert(cantidad_disparos == 14);
    preparacion_y_limite(false);
    preparacion_y_limite(true);
    pulso_corto_y_enlace(false);
    pulso_corto_y_enlace(true);
    assert(cantidad_disparos == 16);
    puts("OK GPIO real: plazos, preparación, pulso corto, tren, cola y reset");
    return 0;
}
