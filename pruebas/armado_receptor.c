/* Adaptador mínimo para ejecutar el receptor real sin construir QEMU. */
#include <assert.h>
#include "hw/misc/taller_armado.h"

#define CABECERA 12
#define ESP32_SENS_CANALES_ADC1 8
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef void (*TallerPuenteReceptor)(void *, uint8_t, const uint8_t *, size_t);
enum {
    TALLER_TRAMA_ENTRADA = 16, TALLER_TRAMA_ANALOGICO = 17,
    TALLER_TRAMA_I2C_DISPOSITIVO = 18, TALLER_TRAMA_I2C_REGISTROS = 19,
    TALLER_TRAMA_ENTRADA_ARMADA = 20,
};
static TallerArmado armado;
static unsigned entradas, analogicos, altas, registros;
static uint8_t bus_esperado = 1;
static uint8_t ultimo_pin;
static bool ultimo_nivel;
static uint16_t lduw_le_p(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static void esp32_gpio_fijar_entrada(uint8_t p, bool n)
{
    entradas++;
    ultimo_pin = p;
    ultimo_nivel = n;
}
static void esp32_sens_fijar_valor(int c, uint16_t v)
{
    assert(c == 6 && v == 2048);
    analogicos++;
}
static void taller_i2c_dispositivo(uint8_t b, uint8_t d, bool a)
{
    assert(b == bus_esperado && d == 0x3c && a);
    altas++;
}
static void taller_i2c_registros(uint8_t b, uint8_t d, uint8_t r,
                                const uint8_t *datos, size_t n)
{
    assert(b == bus_esperado && d == 0x3c && r == 0xd0 && n == 2);
    assert(datos[0] == 0x60 && datos[1] == 0x61);
    registros++;
}
static void esp32_gpio_armar_entrada(const uint8_t *p, size_t n)
{
    taller_armado_recibir(&armado, p, n);
}
#include "clasico_receptor.h"
typedef struct ESP32C3GPIOState { uint32_t in_externo; } ESP32C3GPIOState;
typedef struct ESP32C3SarAdcState { uint16_t valores[5]; } ESP32C3SarAdcState;
typedef struct Esp32C3MachineState {
    ESP32C3GPIOState gpio;
    ESP32C3SarAdcState saradc;
} Esp32C3MachineState;
static void esp32c3_saradc_fijar_valor(ESP32C3SarAdcState *s, uint8_t pin, uint16_t v)
{
    if (pin < 5) {
        s->valores[pin] = MIN(v, 4095);
    }
}
static void esp32c3_gpio_fijar_entrada(ESP32C3GPIOState *s, uint8_t pin, bool nivel)
{
    assert(pin == 9);
    if (nivel) {
        s->in_externo |= 1u << pin;
    } else {
        s->in_externo &= ~(1u << pin);
    }
}
static void esp32c3_gpio_armar_entrada(ESP32C3GPIOState *s, const uint8_t *p, size_t n)
{
    assert(s != NULL);
    taller_armado_recibir(&armado, p, n);
}
#include "c3_receptor.h"
/* Adaptadores de los destinos S3; los modelos se prueban aparte sin QOM. */
typedef struct ESP32S3GPIOState { unsigned llamadas; uint8_t pin; bool nivel; } ESP32S3GPIOState;
typedef struct ESP32S3SarAdcState { unsigned llamadas; uint8_t pin; uint16_t valor; } ESP32S3SarAdcState;
typedef struct Esp32s3SocState {
    ESP32S3GPIOState gpio;
    ESP32S3SarAdcState saradc;
} Esp32s3SocState;
static void esp32s3_gpio_fijar_entrada(ESP32S3GPIOState *s, uint8_t pin, bool nivel)
{
    s->llamadas++;
    s->pin = pin;
    s->nivel = nivel;
}
static void esp32s3_saradc_fijar_valor(ESP32S3SarAdcState *s, uint8_t pin, uint16_t valor)
{
    s->llamadas++;
    s->pin = pin;
    s->valor = valor;
}
static void esp32s3_gpio_armar_entrada(ESP32S3GPIOState *s, const uint8_t *p, size_t n)
{
    assert(s != NULL);
    taller_armado_recibir(&armado, p, n);
}
#include "s3_receptor.h"
#include "puente_receptor.h"

static void recibir(const uint8_t *datos, size_t n, size_t fragmento)
{
    while (n) {
        size_t trozo = MIN(n, fragmento);
        trozo = MIN(trozo, (size_t)puente_puede_leer(NULL));
        assert(trozo > 0);
        puente_leer(NULL, datos, trozo);
        datos += trozo;
        n -= trozo;
    }
}

int main(void)
{
    receptor_entrante = esp32_taller_recibir;
    /* Rutas clásicas reales tras separar el despacho del parser común. */
    uint8_t entrada[] = {16, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 39, 1};
    const uint8_t adc[] = {17, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 34, 0, 8};
    const uint8_t alta[] = {18, 4, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x3c, 1};
    const uint8_t espejo[] = {19, 6, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              1, 0x3c, 0xd0, 0x60, 0x61};
    recibir(entrada, sizeof(entrada), 1);
    recibir(adc, sizeof(adc), 2);
    recibir(alta, sizeof(alta), 3);
    recibir(espejo, sizeof(espejo), sizeof(espejo));
    assert(entradas == 1 && ultimo_pin == 39 && ultimo_nivel);
    assert(analogicos == 1 && altas == 1 && registros == 1);
    for (unsigned tipo = 16; tipo <= 19; tipo++) {
        esp32_taller_recibir(NULL, tipo, NULL, 0);
    }
    assert(entradas == 1 && analogicos == 1 && altas == 1 && registros == 1);

    /* GPIO y ANALOGICO aplicados solamente a la máquina C3 seleccionada. */
    Esp32C3MachineState c3 = {.gpio = {0x3fffff}}, otro_c3 = {.gpio = {0x3fffff}};
    receptor_entrante = esp32c3_taller_recibir;
    receptor_opaque = &c3;
    entrada[12] = 9;
    entrada[13] = 0;
    recibir(entrada, sizeof(entrada), 1);
    recibir(adc, sizeof(adc), 1);
    recibir(alta, sizeof(alta), 1);
    recibir(espejo, sizeof(espejo), 1);
    /* El C3 ignora bus 1, pero entrega las altas y registros de su bus 0. */
    assert(altas == 1 && registros == 1);
    uint8_t alta_c3[sizeof(alta)], espejo_c3[sizeof(espejo)];
    memcpy(alta_c3, alta, sizeof(alta));
    memcpy(espejo_c3, espejo, sizeof(espejo));
    alta_c3[12] = espejo_c3[12] = bus_esperado = 0;
    recibir(alta_c3, sizeof(alta_c3), 1);
    recibir(espejo_c3, sizeof(espejo_c3), 2);
    assert(altas == 2 && registros == 2);
    esp32c3_taller_recibir(&c3, TALLER_TRAMA_I2C_DISPOSITIVO, alta_c3 + 12, 2);
    esp32c3_taller_recibir(&c3, TALLER_TRAMA_I2C_REGISTROS, espejo_c3 + 12, 2);
    assert(altas == 2 && registros == 2);
    assert(!(c3.gpio.in_externo & (1u << 9)) && (otro_c3.gpio.in_externo & (1u << 9)));
    uint8_t adc_c3[] = {17, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8};
    for (unsigned pin = 0; pin <= 4; pin++) {
        adc_c3[12] = pin;
        recibir(adc_c3, sizeof(adc_c3), 1);
        assert(c3.saradc.valores[pin] == 2048 && otro_c3.saradc.valores[pin] == 0);
    }
    adc_c3[12] = 4;
    adc_c3[13] = adc_c3[14] = 255;
    recibir(adc_c3, sizeof(adc_c3), 2);
    assert(c3.saradc.valores[4] == 4095);
    adc_c3[12] = 5; /* ADC2 y los GPIO clásicos se ignoran. */
    recibir(adc_c3, sizeof(adc_c3), 1);
    adc_c3[12] = 34;
    recibir(adc_c3, sizeof(adc_c3), 1);
    for (unsigned tipo = 16; tipo <= 19; tipo++) {
        esp32c3_taller_recibir(&c3, tipo, NULL, 0);
    }
    assert(c3.saradc.valores[0] == 2048 && c3.saradc.valores[4] == 4095);
    receptor_opaque = &otro_c3;
    recibir(entrada, sizeof(entrada), 4);
    assert(!(otro_c3.gpio.in_externo & (1u << 9)));
    assert(entradas == 1 && analogicos == 1 && altas == 2 && registros == 2);
    /* Receptor real S3: pin alto, valor LE y tren, sin tocar otra máquina. */
    Esp32s3SocState s3 = {0}, otro_s3 = {0};
    receptor_entrante = esp32s3_taller_recibir;
    receptor_opaque = &s3;
    entrada[12] = 48;
    entrada[13] = 0;
    recibir(entrada, sizeof(entrada), 1);
    adc_c3[12] = 10;
    adc_c3[13] = 0x34;
    adc_c3[14] = 0x12;
    recibir(adc_c3, sizeof(adc_c3), 2);
    recibir(alta, sizeof(alta), 1);
    recibir(espejo, sizeof(espejo), 1);
    assert(s3.gpio.llamadas == 1 && s3.gpio.pin == 48 && !s3.gpio.nivel);
    assert(s3.saradc.llamadas == 1 && s3.saradc.pin == 10 && s3.saradc.valor == 0x1234);
    assert(!otro_s3.gpio.llamadas && !otro_s3.saradc.llamadas);
    const uint8_t tren_s3[] = {20, 5, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              48, 1, 0, 0, 0, 0, 47, 1, 0, 0, 0, 0, 0, 1};
    taller_armado_iniciar(&armado, 0);
    recibir(tren_s3, sizeof(tren_s3), 1);
    assert(armado.ranuras[0].armada && armado.ranuras[0].programa.pin_disparo == 48);
    assert(armado.ranuras[0].programa.pin_salida == 47);
    for (unsigned tipo = 16; tipo <= 20; tipo++) {
        esp32s3_taller_recibir(&s3, tipo, NULL, 0);
    }
    assert(s3.gpio.llamadas == 1 && s3.saradc.llamadas == 1);
    assert(entradas == 1 && analogicos == 1 && altas == 2 && registros == 2);
    receptor_entrante = esp32_taller_recibir;
    receptor_opaque = NULL;
    /* n=201 no cabe: descartar sus 1026 bytes, incluso si contienen cabeceras. */
    uint8_t excesiva[12 + 9 + 5 * 201] = {20, 5, 0xf6, 3};
    const uint8_t valida[] = {20, 5, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             4, 1, 0, 0, 0, 0, 18, 1, 0, 0, 0, 0, 0, 1};
    excesiva[12 + 7] = 201;
    /* Una cabecera falsa en el resto descartado no debe armar otro pin. */
    memcpy(excesiva + sizeof(excesiva) - 5, valida, 5);
    const size_t fragmentos[] = {1, 12, 127, 1021};
    for (unsigned i = 0; i < sizeof(fragmentos) / sizeof(fragmentos[0]); i++) {
        taller_armado_iniciar(&armado, 0);
        recibir(excesiva, sizeof(excesiva), fragmentos[i]);
        recibir(valida, sizeof(valida), fragmentos[i]);
        assert(armado.ranuras[0].armada);
        assert(armado.ranuras[0].programa.pin_disparo == 4);
        assert(armado.ranuras[0].programa.pin_salida == 18);
        assert(!armado.ranuras[1].armada);
    }
    /* Trama máxima válida, tanto de una vez como fragmentada byte a byte. */
    uint8_t maxima[1021] = {20, 5, 0xf1, 3};
    maxima[12] = maxima[12 + 6] = 39;
    maxima[12 + 7] = 200;
    maxima[1020] = 1;
    for (unsigned i = 0; i < sizeof(fragmentos) / sizeof(fragmentos[0]); i++) {
        taller_armado_iniciar(&armado, 0);
        recibir(maxima, sizeof(maxima), fragmentos[i]);
        assert(armado.ranuras[0].armada);
        assert(armado.ranuras[0].programa.pin_disparo == 39);
        assert(armado.ranuras[0].programa.n == 200);
        assert(armado.ranuras[0].programa.pasos[199].nivel);
    }
    return 0;
}
