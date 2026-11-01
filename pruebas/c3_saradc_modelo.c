/* ADC1 C3 real: polling, canales, saturación, registros y reset. */
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
typedef int qemu_irq;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
static int nivel_irq;
static void qemu_set_irq(qemu_irq irq, int nivel) { nivel_irq = !!nivel; }
/* CABECERA_REAL */
/* MODELO_REAL */
static ESP32C3SarAdcState adc;
static void escribir(hwaddr a, uint32_t v) { esp32c3_saradc_write(&adc, a, v, 4); }
static uint32_t leer(hwaddr a) { return esp32c3_saradc_read(&adc, a, 4); }
static uint32_t convertir(unsigned canal)
{
    escribir(0x4c, 1u << 31);
    escribir(0x20, (1u << 31) | (canal << 25));
    escribir(0x20, (1u << 31) | (canal << 25) | (1u << 29));
    assert(leer(0x44) == (1u << 31));
    return leer(0x2c) & 4095;
}
int main(void)
{
    esp32c3_saradc_reiniciar(&adc, 0);
    for (unsigned c = 0; c < 5; c++) {
        assert(convertir(c) == 0);
        esp32c3_saradc_fijar_valor(&adc, c, 1000 + c);
        assert(convertir(c) == 1000 + c);
    }
    esp32c3_saradc_fijar_valor(&adc, 4, 65535);
    assert(convertir(4) == 4095);
    /* DATA y RAW/ST son de solo lectura. DONE no depende de ENA. */
    escribir(0x2c, 0xffffffff);
    escribir(0x44, 0);
    escribir(0x48, 0xffffffff);
    assert((leer(0x2c) & 4095) == 4095 && leer(0x44) == (1u << 31));
    assert(leer(0x48) == 0 && !nivel_irq);
    escribir(0x40, 1u << 31);
    assert(leer(0x48) == (1u << 31) && nivel_irq);
    escribir(0x4c, 1);
    assert(nivel_irq);                 /* CLR solo limpia los unos escritos. */
    escribir(0x4c, 1u << 31);
    assert(leer(0x44) == 0 && leer(0x48) == 0 && !nivel_irq);
    assert(leer(0x4c) == 0);
    /* Mantener START alto no hace una segunda conversión. */
    esp32c3_saradc_fijar_valor(&adc, 4, 42);
    escribir(0x20, (1u << 31) | (4u << 25) | (1u << 29));
    assert((leer(0x2c) & 4095) == 4095 && leer(0x44) == 0);
    assert(convertir(4) == 42);
    esp32c3_saradc_fijar_valor(&adc, 5, 999);
    esp32c3_saradc_fijar_valor(&adc, 34, 999);
    esp32c3_saradc_fijar_valor(&adc, 255, 999);
    assert(convertir(0) == 1000);
    assert(convertir(5) == 0);         /* Sin canal ADC1 válido, dato cero. */
    escribir(0x4c, 0xffffffff);
    escribir(0x20, 0);
    escribir(0x20, 1u << 29);          /* Sin habilitar ADC1 no hay DONE. */
    assert(leer(0x44) == 0);
    escribir(0x20, 1u << 30);
    escribir(0x20, (1u << 30) | (1u << 29));
    assert(leer(0x44) == 0 && leer(0x30) == 0); /* ADC2 no emulado. */
    escribir(0x0c, 123);
    assert(leer(0x0c) == 123);
    escribir(0x400, 12);
    escribir(0x21, 12);
    assert(leer(0x400) == 0 && leer(0x21) == 0);
    esp32c3_saradc_reiniciar(&adc, 0);
    assert(leer(0x0c) == 0 && leer(0x44) == 0 && !nivel_irq);
    assert(convertir(0) == 1000 && convertir(4) == 42);
    puts("OK ADC1 C3 real: cinco canales, flanco, DATA/DONE, IRQ y reset");
    return 0;
}
