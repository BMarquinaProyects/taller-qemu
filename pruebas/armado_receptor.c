/* Adaptador mínimo para ejecutar el receptor real sin construir QEMU. */
#include <assert.h>
#include "hw/misc/taller_armado.h"

#define CABECERA 12
#define ESP32_SENS_CANALES_ADC1 8
#define MIN(a, b) ((a) < (b) ? (a) : (b))
enum {
    TALLER_TRAMA_ENTRADA = 16, TALLER_TRAMA_ANALOGICO = 17,
    TALLER_TRAMA_I2C_DISPOSITIVO = 18, TALLER_TRAMA_I2C_REGISTROS = 19,
    TALLER_TRAMA_ENTRADA_ARMADA = 20,
};
static TallerArmado armado;
static uint16_t lduw_le_p(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static void esp32_gpio_fijar_entrada(uint8_t p, bool n) {}
static void esp32_sens_fijar_valor(int c, uint16_t v) {}
static void taller_i2c_dispositivo(uint8_t b, uint8_t d, bool a) {}
static void taller_i2c_registros(uint8_t b, uint8_t d, uint8_t r,
                                const uint8_t *datos, size_t n) {}
static void esp32_gpio_armar_entrada(const uint8_t *p, size_t n)
{
    taller_armado_recibir(&armado, p, n);
}
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
