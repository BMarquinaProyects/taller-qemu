/* GPIO S3 real: dos bancos, huecos, matriz de 9 bits y trenes en GPIO48. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_armado.h"
#include "hw/misc/taller_tren.h"
#include "hw/gpio/taller_pull.h"
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef uint64_t hwaddr;
typedef struct QEMUTimer { bool pendiente; int64_t plazo; } QEMUTimer;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
#define REG32(nombre, addr) enum { A_##nombre = addr };
#define QEMU_CLOCK_VIRTUAL 0
static int64_t ahora;
static unsigned muestras, disparos, avisos_matriz;
static uint64_t ultimo_out, ultimo_enable;
static void esp32c3_ledc_actualizar_senal(int senal) { avisos_matriz++; }
static int64_t qemu_clock_get_ns(int reloj) { return ahora; }
static void timer_mod_ns(QEMUTimer *t, int64_t plazo)
{
    t->pendiente = true;
    t->plazo = plazo;
}
static void timer_del(QEMUTimer *t) { t->pendiente = false; }
static bool timer_pending(const QEMUTimer *t) { return t->pendiente; }
static void taller_puente_gpio(uint64_t out, uint64_t enable)
{
    muestras++;
    ultimo_out = out;
    ultimo_enable = enable;
    assert(!((out | enable) & ~0x1fffffc3fffffull));
}
static void taller_puente_disparo(uint8_t pin, int64_t t_ns)
{
    assert(pin == 48 && t_ns == ahora);
    disparos++;
}
static void taller_puente_pull(uint64_t arriba, uint64_t abajo) {}

/* CABECERA_REAL */
/* MODELO_REAL */
static ESP32S3GPIOState gpio;
static QEMUTimer timers[TALLER_ARMADO_RANURAS];
static QEMUTimer timers_tren[TALLER_TREN_RANURAS];
static QEMUTimer cuanto;
static void escribir(hwaddr a, uint32_t v) { esp32s3_gpio_write(&gpio, a, v, 4); }
static uint32_t leer(hwaddr a) { return esp32s3_gpio_read(&gpio, a, 4); }
int main(void)
{
    gpio.strap_mode = 4;
    gpio.cuanto_armado = &cuanto;
    taller_armado_iniciar(&gpio.armado, 0);
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        gpio.trenes[i] = (ESP32S3GpioTren){&timers[i], &gpio, i};
    }
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        gpio.temporizadores_tren[i] = (ESP32S3GpioTren){&timers_tren[i], &gpio, i};
    }
    esp32s3_gpio_reset_hold(&gpio, 0);
    assert(leer(0x38) == 4 && leer(0x3c) == 0xfc3fffff && leer(0x40) == 0x1ffff);
    assert(leer(0x554) == 256 && leer(0x614) == 256);
    escribir(0x4, 0xffffffff);
    escribir(0x10, 0xffffffff);
    escribir(0x20, 0xffffffff);
    escribir(0x2c, 0xffffffff);
    assert(leer(0x4) == 0xfc3fffff && leer(0x10) == 0x1ffff);
    assert(ultimo_out == 0x1fffffc3fffffull && ultimo_enable == ultimo_out);
    escribir(0xc, (1u << 2) | (1u << 31));
    escribir(0x18, (1u << 0) | (1u << 16));
    assert(!(leer(0x3c) & ((1u << 2) | (1u << 31))));
    assert(!(leer(0x40) & ((1u << 0) | (1u << 16))));
    escribir(0x8, 1u << 31);
    escribir(0x14, 1u << 16);
    escribir(0x28, 1u << 4);
    escribir(0x34, 1u << 0);
    assert(!(leer(0x20) & (1u << 4)) && !(leer(0x2c) & 1));
    escribir(0x24, 1u << 4);
    escribir(0x30, 1);
    unsigned antes = muestras;
    escribir(0x8, 0x03c00000); /* Hueco 22..25. */
    escribir(0x14, 0xfffe0000); /* Fuera de GPIO48. */
    assert(muestras == antes);
    esp32s3_gpio_fijar_entrada(&gpio, 48, false);
    assert(leer(0x40) & (1u << 16)); /* Push-pull domina entrada externa. */
    escribir(0x74 + 4 * 48, 0x12340004);
    assert(leer(0x134) == 0x12340004 && !(leer(0x40) & (1u << 16)));
    esp32s3_gpio_fijar_entrada(&gpio, 48, true);
    assert(leer(0x40) & (1u << 16));
    escribir(0x134, 0);
    assert(!(gpio.drenador_abierto & (1ull << 48)));
    escribir(0x554 + 4 * 48, 73 | (1u << 10));
    assert(esp32s3_gpio_pin_de_senal(&gpio, 73) == 48);
    escribir(0x554 + 4 * 2, 73 | (1u << 9));
    assert(esp32s3_gpio_pin_de_senal(&gpio, 73) == 2);
    escribir(0x554 + 4 * 2, 329); /* 329 & 0xff = 73: no debe confundir 9 bits. */
    assert(esp32s3_gpio_pin_de_senal(&gpio, 73) == 48);
    assert(esp32s3_gpio_pin_de_senal(&gpio, 256) == -1);
    escribir(0x554 + 4 * 48, 256);
    assert(esp32s3_gpio_pin_de_senal(&gpio, 73) == -1 && avisos_matriz >= 8);
    for (unsigned p = 22; p <= 25; p++) {
        escribir(0x74 + 4 * p, 4);
        escribir(0x554 + 4 * p, 73);
        assert(leer(0x74 + 4 * p) == 0 && leer(0x554 + 4 * p) == 0);
        esp32s3_gpio_fijar_entrada(&gpio, p, false);
    }
    escribir(0x138, 4); /* PIN49. */
    escribir(0x618, 73);
    assert(leer(0x138) == 0 && leer(0x618) == 0);
    uint64_t in = gpio.in_externo;
    esp32s3_gpio_fijar_entrada(&gpio, 49, false);
    esp32s3_gpio_fijar_entrada(&gpio, 255, true);
    escribir(0x40, 0);
    escribir(0x38, 0);
    assert(in == gpio.in_externo && leer(0x38) == 4);

    uint8_t programa[] = {48, 1, 100, 0, 0, 0, 47, 2, 0,
                         0, 0, 0, 0, 0, 50, 0, 0, 0, 1};
    esp32s3_gpio_reset_hold(&gpio, 0);
    escribir(0x2c, 1u << 16);
    esp32s3_gpio_armar_entrada(&gpio, programa, sizeof(programa));
    assert(gpio.armado.ranuras[0].armada);
    programa[6] = 22;
    esp32s3_gpio_armar_entrada(&gpio, programa, sizeof(programa));
    assert(gpio.armado.ranuras[0].programa.pin_salida == 47);
    programa[0] = 49;
    programa[6] = 47;
    esp32s3_gpio_armar_entrada(&gpio, programa, sizeof(programa));
    assert(!gpio.armado.ranuras[1].armada);
    ahora = 100;
    escribir(0x14, 1u << 16);
    assert(disparos == 1 && !(leer(0x40) & (1u << 15)));
    assert(timers[0].pendiente && timers[0].plazo == 150);
    ahora = 170;
    esp32s3_gpio_paso_armado(&gpio.trenes[0]);
    assert(leer(0x40) & (1u << 15));
    escribir(0x18, 1u << 16);
    ahora += 100;
    escribir(0x14, 1u << 16);
    assert(disparos == 2 && gpio.armado.ranuras[0].sonando);
    esp32s3_gpio_reset_hold(&gpio, 0);
    assert(!timers[0].pendiente && !gpio.armado.ranuras[0].sonando);
    assert(gpio.armado.ranuras[0].armada && disparos == 2);
    assert(leer(0x10) == 0 && leer(0x2c) == 0 && leer(0x40) == 0x1ffff);
    assert(ultimo_out == 0 && ultimo_enable == 0 && leer(0x614) == 256);
    puts("OK GPIO S3 real: bancos, huecos, matriz, drenador abierto, tren GPIO48 y reset");
    return 0;
}
