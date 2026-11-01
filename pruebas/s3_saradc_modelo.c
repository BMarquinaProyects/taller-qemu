/* SENS S3 real: diez canales, START/DONE, dato protegido y reset. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef uint64_t hwaddr;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
/* CABECERA_REAL */
/* MODELO_REAL */
static ESP32S3SarAdcState adc;
static void escribir(hwaddr a, uint32_t v) { esp32s3_saradc_write(&adc, a, v, 4); }
static uint32_t leer(hwaddr a) { return esp32s3_saradc_read(&adc, a, 4); }
static uint32_t convertir(unsigned canal)
{
    uint32_t v = (1u << 31) | (1u << 18) | (1u << (19 + canal));
    escribir(0x0c, v);
    assert(!(leer(0x0c) & (1u << 16)));
    escribir(0x0c, v | (1u << 17));
    assert(leer(0x0c) & (1u << 16));
    return leer(0x0c) & 0xffff;
}

static void calibrar_arranque(void)
{
    /* adc_common.c (IDF 5.5.5) calibra ambas unidades y cuatro atenuaciones
     * antes de setup(). cal_setup() deshabilita todos los pads; después
     * read_cal_channel() pulsa START y espera DONE también en ADC2 (0x30).
     * El cero representa GND interno, sin canales ADC2 externos.
     */
    for (unsigned unidad = 0; unidad < 2; unidad++) {
        hwaddr ctrl = unidad == 0 ? 0x0c : 0x30;
        uint32_t otra_unidad = leer(unidad == 0 ? 0x30 : 0x0c);
        for (unsigned atenuacion = 0; atenuacion < 4; atenuacion++) {
            escribir(unidad == 0 ? 0x14 : 0x38, atenuacion);
            escribir(ctrl, (1u << 31) | (1u << 18)); /* Sin pads. */
            for (unsigned muestra = 0; muestra < 10 * 13; muestra++) {
                assert(!(leer(0x40) & (0xffu << 22)));
                escribir(ctrl, leer(ctrl) & ~(1u << 17));
                assert(!(leer(ctrl) & (1u << 16)));
                escribir(ctrl, leer(ctrl) | (1u << 17));
                assert((leer(ctrl) & 0x1ffff) == (1u << 16));
                /* DATA/DONE protegidos; START alto no repite conversión. */
                escribir(ctrl, leer(ctrl) ^ 0x1ffff);
                assert((leer(ctrl) & 0x1ffff) == (1u << 16));
                assert(leer(unidad == 0 ? 0x30 : 0x0c) == otra_unidad);
            }
        }
    }
}

int main(void)
{
    esp32s3_saradc_reiniciar(&adc, 0);
    calibrar_arranque();
    for (unsigned c = 0; c < 10; c++) {
        assert(convertir(c) == 0);
        esp32s3_saradc_fijar_valor(&adc, c + 1, 1000 + c);
        assert(convertir(c) == 1000 + c);
    }
    esp32s3_saradc_fijar_valor(&adc, 10, 65535);
    assert(convertir(9) == 4095);
    esp32s3_saradc_fijar_valor(&adc, 10, 42);
    uint32_t anterior = leer(0x0c);
    escribir(0x0c, anterior ^ 0x1ffff); /* DATA/DONE no son escribibles. */
    assert((leer(0x0c) & 0x1ffff) == (anterior & 0x1ffff));
    assert(convertir(9) == 42);
    esp32s3_saradc_fijar_valor(&adc, 0, 999);
    esp32s3_saradc_fijar_valor(&adc, 11, 999);
    esp32s3_saradc_fijar_valor(&adc, 255, 999);
    assert(convertir(0) == 1000);
    assert(convertir(10) == 0); /* Pads 10/11 no son canales ADC1 S3. */
    escribir(0x0c, 0);
    escribir(0x0c, 1u << 17); /* Sin pad: termina con cero, sin atascar polling. */
    assert((leer(0x0c) & 0x1ffff) == (1u << 16));
    escribir(0x40, 0xffffffff); /* MEAS_STATUS [29:22] siempre libre. */
    assert(!(leer(0x40) & (0xffu << 22)));
    escribir(0x18, 123);
    assert(leer(0x18) == 123);
    escribir(0x400, 1);
    escribir(0x0d, 1);
    assert(leer(0x400) == 0 && leer(0x0d) == 0);
    esp32s3_saradc_reiniciar(&adc, 0);
    assert(leer(0x0c) == 0 && leer(0x30) == 0 && leer(0x18) == 0);
    calibrar_arranque(); /* GND sigue en cero aunque ADC1 tenga entradas. */
    assert(convertir(0) == 1000 && convertir(9) == 42);
    puts("OK ADC S3 real: calibración ADC1/ADC2, diez canales ADC1, START/DONE y reset");
    return 0;
}
