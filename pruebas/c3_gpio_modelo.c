/* Ejecuta registros y temporizadores del GPIO C3 real, sin servicios QOM. */
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
static unsigned muestras, disparos;
static uint64_t ultimo_out, ultimo_enable;
static unsigned avisos_matriz;
static void esp32c3_ledc_actualizar_senal(int senal) { avisos_matriz++; }
static int64_t qemu_clock_get_ns(int reloj) { return ahora; }
static void timer_mod_ns(QEMUTimer *t, int64_t plazo)
{
    t->pendiente = true;
    t->plazo = plazo;
}
static void timer_del(QEMUTimer *t) { t->pendiente = false; }
static void taller_puente_gpio(uint64_t out, uint64_t enable)
{
    muestras++;
    ultimo_out = out;
    ultimo_enable = enable;
    assert(!((out | enable) >> 22));
}
static void taller_puente_disparo(uint8_t pin, int64_t t_ns)
{
    assert(pin == 8 && t_ns == ahora);
    disparos++;
}

static void taller_puente_pull(uint64_t arriba, uint64_t abajo) {}

/* CABECERA_REAL */
/* MODELO_REAL */

static ESP32C3GPIOState gpio;
static QEMUTimer timers[TALLER_ARMADO_RANURAS];
static QEMUTimer timers_tren[TALLER_TREN_RANURAS];
static void escribir(hwaddr addr, uint32_t v) { esp32c3_gpio_write(&gpio, addr, v, 4); }
static uint32_t leer(hwaddr addr) { return esp32c3_gpio_read(&gpio, addr, 4); }

int main(void)
{
    const uint32_t b8 = 1u << 8, b9 = 1u << 9, b21 = 1u << 21;
    gpio.strap_mode = 8;
    taller_armado_iniciar(&gpio.armado, 0);
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        gpio.trenes[i] = (ESP32C3GpioTren){&timers[i], &gpio, i};
    }
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        gpio.temporizadores_tren[i] = (ESP32C3GpioTren){&timers_tren[i], &gpio, i};
    }
    esp32c3_gpio_reset_hold(&gpio, 0);
    assert(leer(0x38) == 8 && leer(0x3c) == 0x3fffff);
    assert(leer(0x554) == 128 && leer(0x5a8) == 128);
    escribir(0x4, 0xffffffff);
    assert(leer(0x4) == 0x3fffff && ultimo_out == 0x3fffff);
    escribir(0x20, 0xffffffff);
    assert(leer(0x20) == 0x3fffff && ultimo_enable == 0x3fffff);
    escribir(0xc, b8 | b21);
    assert(!(leer(0x3c) & (b8 | b21)));
    escribir(0x8, b8);
    assert((leer(0x4) & (b8 | b21)) == b8);
    escribir(0x28, b9 | b21);
    assert(!(leer(0x20) & (b9 | b21)));
    escribir(0x24, b21);
    assert(leer(0x20) & b21);
    unsigned previas = muestras;
    escribir(0x8, b8 | (1u << 22));
    escribir(0xc, 1u << 22);
    escribir(0x24, 1u << 31);
    escribir(0x28, 1u << 31);
    assert(muestras == previas);
    esp32c3_gpio_fijar_entrada(&gpio, 9, false);
    assert(!(leer(0x3c) & b9));
    esp32c3_gpio_fijar_entrada(&gpio, 9, true);
    assert(leer(0x3c) & b9);
    // Push-pull alto ignora el cero externo; PAD_DRIVER lo deja pasar.
    esp32c3_gpio_fijar_entrada(&gpio, 8, false);
    assert(leer(0x3c) & b8);
    escribir(0x74 + 4 * 8, 0x12340004);
    assert(leer(0x94) == 0x12340004 && !(leer(0x3c) & b8));
    esp32c3_gpio_fijar_entrada(&gpio, 8, true);
    assert(leer(0x3c) & b8);
    escribir(0xc, b8);
    assert(!(leer(0x3c) & b8));
    escribir(0x94, 0);
    assert(!(gpio.drenador_abierto & b8));
    escribir(0x74, 4);
    escribir(0xc8, 4);
    assert(gpio.drenador_abierto == ((1u << 0) | b21));
    escribir(0xcc, 4);  // PIN22 no existe.
    escribir(0x554, 0xdeadbeef);
    escribir(0x5a8, 0x12345678);
    escribir(0x5ac, 0xffffffff);
    assert(leer(0x554) == 0xdeadbeef && leer(0x5a8) == 0x12345678);
    escribir(0x554 + 4 * 8, 45 | (1u << 8));
    escribir(0x554 + 4 * 21, 45);
    assert(esp32c3_gpio_pin_de_senal(&gpio, 45) == 8);
    escribir(0x554 + 4 * 8, 128);
    assert(esp32c3_gpio_pin_de_senal(&gpio, 45) == 21);
    assert(esp32c3_gpio_pin_de_senal(&gpio, 50) == -1);
    assert(esp32c3_gpio_pin_de_senal(&gpio, 128) == -1 && avisos_matriz >= 6);
    assert(leer(0x5ac) == 0);
    uint32_t in = gpio.in_externo;
    esp32c3_gpio_fijar_entrada(&gpio, 22, false);
    esp32c3_gpio_fijar_entrada(&gpio, 255, true);
    assert(gpio.in_externo == in);
    escribir(0x3c, 0);  // IN y STRAP son de solo lectura.
    escribir(0x38, 0);
    assert(gpio.in_externo == in && leer(0x38) == 8);

    // Tren: flanco de subida tras 100 ns bajos, primer paso cero, segundo +50.
    uint8_t programa[] = {8, 1, 100, 0, 0, 0, 9, 2, 0,
                         0, 0, 0, 0, 0, 50, 0, 0, 0, 1};
    esp32c3_gpio_reset_hold(&gpio, 0);
    escribir(0x20, b8);
    esp32c3_gpio_armar_entrada(&gpio, programa, sizeof(programa));
    assert(gpio.armado.ranuras[0].armada);
    programa[6] = 22;
    esp32c3_gpio_armar_entrada(&gpio, programa, sizeof(programa));
    assert(gpio.armado.ranuras[0].programa.pin_salida == 9);
    programa[0] = 22;
    programa[6] = 9;
    esp32c3_gpio_armar_entrada(&gpio, programa, sizeof(programa));
    assert(!gpio.armado.ranuras[1].armada);
    ahora = 100;
    escribir(0x8, b8);
    assert(disparos == 1 && !(leer(0x3c) & b9));
    assert(timers[0].pendiente && timers[0].plazo == 150);
    ahora = 149;
    esp32c3_gpio_paso_armado(&gpio.trenes[0]);
    assert(!(leer(0x3c) & b9));
    ahora = 170;  // Callback tardío conserva el plazo absoluto.
    esp32c3_gpio_paso_armado(&gpio.trenes[0]);
    assert(leer(0x3c) & b9);
    assert(!gpio.armado.ranuras[0].sonando);
    escribir(0xc, b8);
    ahora += 100;
    escribir(0x8, b8);
    assert(disparos == 2 && gpio.armado.ranuras[0].sonando);
    esp32c3_gpio_reset_hold(&gpio, 0);
    assert(!timers[0].pendiente && !gpio.armado.ranuras[0].sonando);
    assert(gpio.armado.ranuras[0].armada && disparos == 2);
    assert(leer(0x4) == 0 && leer(0x20) == 0 && leer(0x3c) == 0x3fffff);
    assert(leer(0x94) == 0 && leer(0x554) == 128);
    assert(ultimo_out == 0 && ultimo_enable == 0);
    puts("OK GPIO C3 real: registros, 22 pines, drenador abierto, trenes y reset");
    return 0;
}
