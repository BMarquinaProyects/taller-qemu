/* Registros LEDC C3/S3 reales; solo se sustituyen QOM, matriz y transporte. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef uint64_t hwaddr;
#ifdef MODELO_S3
#define CANALES_PRUEBA 8
#define SENAL_PRUEBA 73
#define PINES_PRUEBA 49
#else
#define CANALES_PRUEBA 6
#define SENAL_PRUEBA 45
#define PINES_PRUEBA 22
#endif
typedef struct ESP32C3GPIOState { int rutas[256]; } ESP32C3GPIOState;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
static int resolver_senal(void *opaque, int senal)
{
    ESP32C3GPIOState *s = opaque;
    return s ? s->rutas[senal] : -1;
}
static struct { int gpio; uint32_t frecuencia, ciclo; } muestras[256];
static unsigned n;
static void taller_puente_pwm(int gpio, uint32_t f, uint32_t c)
{
    assert(n < 256 && gpio >= 0 && gpio < PINES_PRUEBA && c <= 1000000);
    assert(f || !c);
    muestras[n].gpio = gpio;
    muestras[n].frecuencia = f;
    muestras[n++].ciclo = c;
}
/* CABECERA_REAL */
/* MODELO_REAL */
static ESP32C3LEDCState ledc;
static ESP32C3GPIOState gpio;
static void escribir(hwaddr a, uint32_t v) { esp32c3_ledc_write(&ledc, a, v, 4); }
static uint32_t leer(hwaddr a) { return esp32c3_ledc_read(&ledc, a, 4); }
int main(void)
{
    memset(&gpio, 0xff, sizeof(gpio));
    ledc.gpio = &gpio;
    ledc.canales = CANALES_PRUEBA;
    ledc.senal0 = SENAL_PRUEBA;
    ledc.pin_de_senal = resolver_senal;
    ledc_del_chip = &ledc;
    esp32c3_ledc_reiniciar(&ledc, 0);
    assert(n == 0);
    escribir(0xd0, 3);                 /* XTAL 40 MHz; 50 Hz a 14 bits. */
    escribir(0xa0, 14 | (12500u << 4));
    escribir(0x08, 819u << 4);
    escribir(0x00, 4);
    assert(n == 0);                    /* Sin matriz todavía. */
    gpio.rutas[SENAL_PRUEBA] = 8;
    esp32c3_ledc_actualizar_senal(SENAL_PRUEBA);
    assert(n == 1 && muestras[0].gpio == 8);
    assert(muestras[0].frecuencia == 50000 && muestras[0].ciclo == 49987);
    escribir(0x00, 4 | 16);            /* PARA_UP no duplica el PWM. */
    escribir(0xa0, 14 | (12500u << 4) | (1u << 25));
    escribir(0x0c, 1u << 31);
    assert(!(leer(0x0c) & (1u << 31)) && n == 1);
    assert(leer(0x10) == (819u << 4));
    escribir(0x10, 0xffffffff);         /* DUTY_R es de solo lectura. */
    assert(leer(0x10) == (819u << 4));
    escribir(0x08, 4096u << 4);
    assert(muestras[n - 1].ciclo == 250000);
    escribir(0xa0, 14 | (6250u << 4));
    assert(muestras[n - 1].frecuencia == 100000);
    escribir(0xd0, 1);                 /* APB: mismo divisor, doble frecuencia. */
    assert(muestras[n - 1].frecuencia == 200000);
    escribir(0xd0, 2);                 /* RC_FAST nominal de 17,5 MHz. */
    assert(muestras[n - 1].frecuencia == 43750);
    escribir(0xd0, 0);
    assert(muestras[n - 1].frecuencia == 0 && muestras[n - 1].ciclo == 0);
    escribir(0xd0, 3);
    escribir(0xa0, 14 | (6250u << 4) | (1u << 22));
    assert(muestras[n - 1].ciclo == 0); /* Timer pausado. */
    escribir(0xa0, 14 | (6250u << 4) | (1u << 23));
    assert(muestras[n - 1].ciclo == 0); /* Timer en reset. */
    escribir(0xa0, 14 | (6250u << 4));
    unsigned antes = n;
    gpio.rutas[SENAL_PRUEBA] = 9;
    esp32c3_ledc_actualizar_senal(SENAL_PRUEBA);
    assert(n == antes + 2 && muestras[n - 2].gpio == 8 && !muestras[n - 2].ciclo);
    assert(muestras[n - 1].gpio == 9 && muestras[n - 1].ciclo == 250000);
    gpio.rutas[SENAL_PRUEBA] = -1;
    esp32c3_ledc_actualizar_senal(SENAL_PRUEBA);
    assert(muestras[n - 1].gpio == 9 && !muestras[n - 1].ciclo);
    /* Todos los canales y cuatro timers, incluidos sus extremos. */
    for (unsigned c = 0; c < CANALES_PRUEBA; c++) {
        unsigned t = c % 4;
        gpio.rutas[SENAL_PRUEBA + c] = 10 + c;
        escribir(0xa0 + 8 * t, 10 | (10000u << 4));
        escribir(0x14 * c + 8, 512u << 4);
        escribir(0x14 * c, t | 4);
        esp32c3_ledc_actualizar_senal(SENAL_PRUEBA + c);
        assert(muestras[n - 1].gpio == (int)(10 + c));
        assert(muestras[n - 1].frecuencia == 1000000 && muestras[n - 1].ciclo == 500000);
    }
    escribir(0x08, 1024u << 4);
    assert(muestras[n - 1].ciclo == 1000000);
    escribir(0xa0, 10);                /* Divisor cero y resolución inválida. */
    assert(!muestras[n - 1].ciclo);
    escribir(0xa0, 15 | (256u << 4));
    assert(!muestras[n - 1].ciclo);
    escribir(0xa0, 1 | (1u << 4));
    assert(muestras[n - 1].frecuencia == UINT32_MAX);
    antes = n;
    hwaddr hueco = CANALES_PRUEBA == 6 ? 0x78 : 0xd4;
    escribir(hueco, 0xffffffff);        /* Hueco, fuera de rango y desalineado. */
    escribir(0x100, 0xffffffff);
    escribir(0x09, 0xffffffff);
    assert(leer(hueco) == 0 && leer(0x100) == 0 && leer(0x09) == 0 && n == antes);
    escribir(0x00, 0);
    assert(!muestras[n - 1].ciclo);
    esp32c3_ledc_reiniciar(&ledc, 0);
    assert(leer(0x08) == 0 && leer(0x00) == 0);
    assert(!muestras[n - 1].ciclo);
    printf("OK LEDC LS real: %u canales, relojes, timers, matriz, parada y reset\n",
           CANALES_PRUEBA);
    return 0;
}
