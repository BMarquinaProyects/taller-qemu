/* GPIO clásico real con reloj y temporizadores dobles, sin ejecutar QEMU. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_armado.h"

typedef int SysBusDevice;
typedef int SysBusDeviceClass;
typedef int MemoryRegion;
typedef int qemu_irq;
typedef uint64_t hwaddr;
typedef struct QEMUTimer { int64_t plazo; } QEMUTimer;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
#define REG32(nombre, addr) enum { A_##nombre = addr };
#define FIELD(reg, nombre, pos, ancho)
#define FIELD_EX32(valor, reg, nombre) ((valor) & 0x1ff)
#define QEMU_CLOCK_VIRTUAL 1
static int64_t ahora;
static unsigned cantidad_disparos;

static int64_t qemu_clock_get_ns(int reloj)
{
    assert(reloj == QEMU_CLOCK_VIRTUAL);
    return ahora;
}
static void timer_mod_ns(QEMUTimer *t, int64_t plazo) { t->plazo = plazo; }
static void esp32_ledc_actualizar_senal(int senal) { (void)senal; }
static void taller_puente_gpio(uint64_t out, uint64_t enable)
{
    (void)out;
    (void)enable;
}
static void taller_puente_disparo(uint8_t pin, int64_t t_ns)
{
    assert(pin == 4 && t_ns == 1000000);
    cantidad_disparos++;
}

/* CABECERA_REAL */
/* MODELO_REAL */

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
    QEMUTimer timers[TALLER_ARMADO_RANURAS] = {0};
    int64_t flancos[6];

    ahora = 0;
    gpio_del_chip = &gpio;
    gpio.in_externo = UINT64_MAX;
    taller_armado_iniciar(&gpio.armado, ahora);
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        gpio.trenes[i] = (Esp32GpioTemporizadorArmado){&timers[i], &gpio, i};
    }
    /* La ruta MMIO real suelta GPIO4 tras 1 ms bajo y programa el tren. */
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TS, 1u << 4, 4);
    esp32_gpio_armar_entrada(carga, sizeof(carga));
    ahora = 1000000;
    esp32_gpio_write(&gpio, A_GPIO_ENABLE_W1TC, 1u << 4, 4);
    assert(timers[0].plazo == 1030000);
    for (unsigned i = 0; i < 6; i++) {
        assert(timers[0].plazo == previstos[i]);
        ahora = previstos[i] + (i == paso_tardio ? 4000 : 0);
        esp32_gpio_paso_armado(&gpio.trenes[0]);
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
}

int main(void)
{
    comprobar_tren(0);
    comprobar_tren(4);
    assert(cantidad_disparos == 2);
    puts("OK GPIO clásico: plazos fijos y efecto de atender un flanco 4 us tarde");
    return 0;
}
