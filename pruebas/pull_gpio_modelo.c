/* Ejecuta los GPIO reales con servicios QEMU mínimos, sin emular su lógica. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int SysBusDevice;
typedef int SysBusDeviceClass;
typedef int MemoryRegion;
typedef int qemu_irq;
typedef void Object;
typedef void DeviceState;
typedef int ResetType;
typedef int Error;
typedef uint64_t hwaddr;
typedef struct QEMUTimer { bool pendiente; int64_t plazo; } QEMUTimer;
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
#define QEMU_CLOCK_VIRTUAL 0
static int64_t ahora;
static Error *error_fatal;
static unsigned avisos;
static uint64_t ultimo_arriba, ultimo_abajo;
static QEMUTimer timers[9];
static unsigned usados;
static int64_t qemu_clock_get_ns(int reloj) { return ahora; }
static QEMUTimer *timer_new_ns(int reloj, void (*cb)(void *), void *op)
{
    assert(usados < 9);
    return &timers[usados++];
}
static void timer_mod_ns(QEMUTimer *t, int64_t plazo)
{ t->pendiente = true; t->plazo = plazo; }
static void timer_del(QEMUTimer *t) { t->pendiente = false; }
static void timer_free(QEMUTimer *t) { timer_del(t); }
#ifndef CHIP_ESP32C3
static bool timer_pending(QEMUTimer *t) { return t->pendiente; }
#endif
#ifdef CHIP_ESP32
static void esp32_ledc_actualizar_senal(int senal) {}
static void sysbus_init_irq(SysBusDevice *s, qemu_irq *irq) {}
#else
static void esp32c3_ledc_actualizar_senal(int senal) {}
#endif
static void object_property_set_int(Object *o, const char *n, int v, Error **e) {}
static void memory_region_init_io(MemoryRegion *m, Object *o,
    const MemoryRegionOps *ops, void *s, const char *n, uint64_t largo) {}
static void sysbus_init_mmio(SysBusDevice *s, MemoryRegion *m) {}
static void taller_puente_gpio(uint64_t out, uint64_t enable) {}
static void taller_puente_disparo(uint8_t pin, int64_t ns) {}
static void taller_puente_pull(uint64_t arriba, uint64_t abajo)
{
    assert(arriba != ultimo_arriba || abajo != ultimo_abajo);
    ultimo_arriba = arriba;
    ultimo_abajo = abajo;
    avisos++;
}

/* CABECERA_REAL */
/* MODELO_REAL */

#ifdef CHIP_ESP32
typedef Esp32GpioState Estado;
#define iniciar esp32_gpio_init
#define reiniciar esp32_gpio_reset_hold
#define finalizar esp32_gpio_finalize
#define entrada(s, p, n) esp32_gpio_fijar_entrada(p, n)
#define armar(s, p, n) esp32_gpio_armar_entrada(p, n)
#define escribir esp32_gpio_write
#define leer esp32_gpio_read
#define mux_w esp32_iomux_write
#define mux_r esp32_iomux_read
#define REG_PAD 0x3c
#define PIN_PRUEBA 15
#elif defined(CHIP_ESP32C3)
typedef ESP32C3GPIOState Estado;
#define iniciar esp32c3_gpio_init
#define reiniciar esp32c3_gpio_reset_hold
#define finalizar esp32c3_gpio_finalize
#define entrada esp32c3_gpio_fijar_entrada
#define armar esp32c3_gpio_armar_entrada
#define escribir esp32c3_gpio_write
#define leer esp32c3_gpio_read
#define mux_w esp32c3_iomux_write
#define mux_r esp32c3_iomux_read
#define REG_PAD 0x28
#define PIN_PRUEBA 9
#else
typedef ESP32S3GPIOState Estado;
#define iniciar esp32s3_gpio_init
#define reiniciar esp32s3_gpio_reset_hold
#define finalizar esp32s3_gpio_finalize
#define entrada esp32s3_gpio_fijar_entrada
#define armar esp32s3_gpio_armar_entrada
#define escribir esp32s3_gpio_write
#define leer esp32s3_gpio_read
#define mux_w esp32s3_iomux_write
#define mux_r esp32s3_iomux_read
#define REG_PAD 0x40
#define PIN_PRUEBA 15
#endif

int main(void)
{
    Estado s = {0};
    const uint64_t bit = 1ull << PIN_PRUEBA;
#ifdef CHIP_ESP32
    (void)esp32_gpio_realize;
#endif
    iniciar(&s);
    reiniciar(&s, 0);
    assert(!s.pull_arriba && !s.pull_abajo && !avisos);
    /* Cuatro casos eléctricos y conservación de todos los bits del registro. */
    entrada(&s, PIN_PRUEBA, 0);
    entrada(&s, PIN_PRUEBA, 2);
    assert((s.sueltos & bit) && !(leer(&s, 0x3c, 4) & bit));
    mux_w(&s, REG_PAD, 0xabcd0100u, 4);
    assert(mux_r(&s, REG_PAD, 4) == 0xabcd0100u);
    assert(s.pull_arriba == bit && (leer(&s, 0x3c, 4) & bit));
    unsigned antes = avisos;
    mux_w(&s, REG_PAD, 0x100, 4);
    assert(avisos == antes);
    mux_w(&s, REG_PAD, 0x180, 4);
    assert(s.pull_abajo == bit && (leer(&s, 0x3c, 4) & bit));
    mux_w(&s, REG_PAD, 0x80, 4);
    assert(!(leer(&s, 0x3c, 4) & bit));
    mux_w(&s, REG_PAD, 0, 4);
    assert(!(leer(&s, 0x3c, 4) & bit));
    entrada(&s, PIN_PRUEBA, 1);
    assert(!(s.sueltos & bit));
    mux_w(&s, REG_PAD, 0x80, 4);
    assert(leer(&s, 0x3c, 4) & bit); /* Impuesto: ignora PD. */
    entrada(&s, PIN_PRUEBA, 2);
    assert(!(leer(&s, 0x3c, 4) & bit));
    entrada(&s, PIN_PRUEBA, 0);
    assert(!(s.sueltos & bit));
    mux_w(&s, REG_PAD, 0x100, 4);
    assert(!(leer(&s, 0x3c, 4) & bit)); /* Impuesto: ignora PU. */
    entrada(&s, PIN_PRUEBA, 2);
    assert(leer(&s, 0x3c, 4) & bit);
    mux_w(&s, REG_PAD, 0, 4);
    assert(leer(&s, 0x3c, 4) & bit); /* Sin pull conserva también 1. */
    mux_w(&s, REG_PAD, 0x80, 4);
    reiniciar(&s, 0);
    assert(s.sueltos & bit);
    assert(!(s.in_externo & bit) && !s.pull_arriba && !s.pull_abajo);
    assert(mux_r(&s, REG_PAD, 4) == 0);

#ifdef CHIP_ESP32
    /* Offsets contrastados con GPIO_PIN_MUX_REG de IDF, no con la tabla usada. */
    const unsigned pines[] = {0, 2, 4, 15, 16, 34, 36};
    const unsigned offsets[] = {0x44, 0x40, 0x48, 0x3c, 0x4c, 0x14, 0x04};
    for (unsigned i = 0; i < 7; i++) {
        mux_w(&s, offsets[i], 0x180, 4);
        uint64_t esperado = pines[i] >= 34 ? 0 : 1ull << pines[i];
        assert(s.pull_arriba == esperado && s.pull_abajo == esperado);
        assert(mux_r(&s, offsets[i], 4) == 0x180);
        mux_w(&s, offsets[i], 0, 4);
    }
    /* GPIO15 por RTC_IO; OR digital/RTC y dos pads en un mismo registro. */
    esp32_rtcio_write(&s, 0xa0, 1u << 27, 4);
    assert(s.pull_arriba == bit && (s.in_externo & bit));
    assert(esp32_rtcio_read(&s, 0xa0, 4) == (1u << 27));
    mux_w(&s, REG_PAD, 0x80, 4);
    assert(s.pull_arriba == bit && s.pull_abajo == bit && (s.in_externo & bit));
    esp32_rtcio_write(&s, 0xa0, 0, 4);
    assert(!s.pull_arriba && !(s.in_externo & bit));
    esp32_rtcio_write(&s, 0x8c, (1u << 27) | (1u << 23), 4);
    assert(s.pull_arriba == (1ull << 33));
    assert(s.pull_abajo == ((1ull << 32) | bit));
    entrada(&s, 34, 0);
    entrada(&s, 34, 2);
    esp32_rtcio_write(&s, 0x80, 0xffffffffu, 4);
    mux_w(&s, 0x14, 0xffffffffu, 4);
    assert(!((s.pull_arriba | s.pull_abajo | s.in_externo) & (1ull << 34)));
    reiniciar(&s, 0);
    assert(!esp32_rtcio_read(&s, 0xa0, 4) && !esp32_rtcio_read(&s, 0x8c, 4));
#else
    /* Extremos y huecos: no se truncan las máscaras altas en Windows. */
    unsigned ultimo =
#ifdef CHIP_ESP32C3
        21;
#else
        48;
    mux_w(&s, 4 + 4 * 22, 0x180, 4);
    assert(!s.pull_arriba && !s.pull_abajo);
    entrada(&s, 22, 2);
    assert(!(s.sueltos & (1ull << 22)));
#endif
    entrada(&s, ultimo, 0);
    entrada(&s, ultimo, 2);
    mux_w(&s, 4 + 4 * ultimo, 0x100, 4);
    assert(s.pull_arriba == (1ull << ultimo));
    assert(s.in_externo & (1ull << ultimo));
    entrada(&s, 255, 2); /* Índice fuera del chip: sin desplazamientos inválidos. */
#endif
    /* Un paso armado retira sueltos mediante la misma función que ENTRADA. */
    entrada(&s, PIN_PRUEBA, 2);
    uint8_t programa[] = {4, 1, 0, 0, 0, 0, PIN_PRUEBA, 1, 0, 0, 0, 0, 0, 0};
    escribir(&s, 0x20, 1u << 4, 4);
    armar(&s, programa, sizeof(programa));
    ahora = 100;
    escribir(&s, 0x08, 1u << 4, 4);
    assert(!(s.sueltos & bit) && !(s.in_externo & bit));
    finalizar(&s);
    puts("OK pull: registros, tablas, niveles, reset y armados reales");
    return 0;
}
