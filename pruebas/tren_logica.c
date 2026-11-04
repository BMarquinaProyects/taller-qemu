/* Casos de la lógica pura de ENTRADA_TREN (v8), la misma que usan los tres GPIO. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_tren.h"

#define CLASICO UINT64_C(0xffffffffff)
#define S3 UINT64_C(0x1fffffc3fffff)

/* pin, n y n pasos (espera_ns LE, nivel). */
static size_t carga(uint8_t *b, uint8_t pin, uint16_t n, const uint32_t *esperas,
                    const uint8_t *niveles)
{
    b[0] = pin;
    b[1] = (uint8_t)n;
    b[2] = (uint8_t)(n >> 8);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = b + 3 + 5 * i;
        p[0] = (uint8_t)esperas[i];
        p[1] = (uint8_t)(esperas[i] >> 8);
        p[2] = (uint8_t)(esperas[i] >> 16);
        p[3] = (uint8_t)(esperas[i] >> 24);
        p[4] = niveles[i];
    }
    return 3 + 5 * (size_t)n;
}

static void pasos_relativos_y_esperas_cero(void)
{
    TallerTren s;
    uint8_t b[TALLER_TREN_CARGA_MAX], pin, nivel;
    const uint32_t esperas[] = {0, 300000, 300000, 0};
    const uint8_t niveles[] = {0, 2, 0, 1};
    int cancelada;
    taller_tren_iniciar(&s);
    size_t largo = carga(b, 16, 4, esperas, niveles);
    int r = taller_tren_recibir(&s, b, largo, CLASICO, 1000, &cancelada);
    assert(r == 0 && cancelada == -1);
    /* Espera cero: el primer paso vence en la recepción. */
    assert(taller_tren_avanzar(&s.ranuras[0], 1000, &pin, &nivel));
    assert(pin == 16 && nivel == 0);
    assert(!taller_tren_avanzar(&s.ranuras[0], 300999, &pin, &nivel));
    assert(taller_tren_avanzar(&s.ranuras[0], 301000, &pin, &nivel) && nivel == 2);
    /* Plazos acumulativos: una atención tardía no desplaza los siguientes. */
    assert(taller_tren_avanzar(&s.ranuras[0], 650000, &pin, &nivel) && nivel == 0);
    assert(s.ranuras[0].proximo_ns == 601000);
    assert(taller_tren_avanzar(&s.ranuras[0], 650000, &pin, &nivel) && nivel == 1);
    assert(!s.ranuras[0].activa);
    assert(!taller_tren_avanzar(&s.ranuras[0], 9999999, &pin, &nivel));
}

static void cargas_invalidas_se_ignoran_enteras(void)
{
    TallerTren s;
    uint8_t b[TALLER_TREN_CARGA_MAX + 8];
    uint32_t esperas[TALLER_TREN_PASOS + 1] = {0};
    uint8_t niveles[TALLER_TREN_PASOS + 1] = {0};
    int cancelada;
    taller_tren_iniciar(&s);
    size_t largo = carga(b, 4, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == 0);
    /* Largo que no cuadra, corto, sobrante. */
    assert(taller_tren_recibir(&s, b, largo - 1, CLASICO, 0, &cancelada) == -1);
    assert(taller_tren_recibir(&s, b, largo + 1, CLASICO, 0, &cancelada) == -1);
    assert(taller_tren_recibir(&s, b, 2, CLASICO, 0, &cancelada) == -1);
    /* n por encima de 64. */
    largo = carga(b, 4, TALLER_TREN_PASOS + 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == -1);
    /* Nivel 3 en el último paso. */
    niveles[1] = 3;
    largo = carga(b, 4, 2, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == -1);
    niveles[1] = 0;
    /* Pines fuera del chip: 40 en el clásico, 22..25 y 49 en el S3. */
    largo = carga(b, 40, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == -1);
    for (uint8_t pin = 22; pin <= 25; pin++) {
        largo = carga(b, pin, 1, esperas, niveles);
        assert(taller_tren_recibir(&s, b, largo, S3, 0, &cancelada) == -1);
    }
    largo = carga(b, 49, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, S3, 0, &cancelada) == -1);
    largo = carga(b, 200, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, S3, 0, &cancelada) == -1);
    /* Nada de lo anterior tocó el tren válido del GPIO4. */
    assert(s.ranuras[0].activa && s.ranuras[0].pin == 4);
    for (unsigned i = 1; i < TALLER_TREN_RANURAS; i++) {
        assert(!s.ranuras[i].activa);
    }
    /* El máximo, 64 pasos, sí vale. */
    largo = carga(b, 48, TALLER_TREN_PASOS, esperas, niveles);
    assert(largo == TALLER_TREN_CARGA_MAX);
    assert(taller_tren_recibir(&s, b, largo, S3, 0, &cancelada) == 1);
}

static void sustitucion_y_cancelacion(void)
{
    TallerTren s;
    uint8_t b[TALLER_TREN_CARGA_MAX], pin, nivel;
    const uint32_t esperas[] = {1000, 1000};
    const uint8_t a[] = {0, 1}, z[] = {2, 2};
    int cancelada;
    taller_tren_iniciar(&s);
    size_t largo = carga(b, 15, 2, esperas, a);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == 0);
    /* Un tren nuevo en el mismo pin reutiliza la ranura y descarta lo pendiente. */
    largo = carga(b, 15, 2, esperas, z);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 500, &cancelada) == 0);
    assert(cancelada == 0 && s.ranuras[0].proximo_ns == 1500);
    assert(taller_tren_avanzar(&s.ranuras[0], 1500, &pin, &nivel) && nivel == 2);
    /* ENTRADA cancela el tren del pin, no los de otros pines. */
    largo = carga(b, 16, 2, esperas, a);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == 1);
    assert(taller_tren_cancelar(&s, 15) == 0 && !s.ranuras[0].activa);
    assert(taller_tren_cancelar(&s, 15) == -1);
    assert(s.ranuras[1].activa);
    /* n = 0 solo cancela. */
    largo = carga(b, 16, 0, esperas, a);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == -2);
    assert(cancelada == 1 && !s.ranuras[1].activa);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == -2);
    assert(cancelada == -1);
}

static void ocho_ranuras_y_novena(void)
{
    TallerTren s;
    uint8_t b[TALLER_TREN_CARGA_MAX];
    const uint32_t esperas[] = {1000};
    const uint8_t niveles[] = {1};
    int cancelada;
    taller_tren_iniciar(&s);
    for (uint8_t pin = 0; pin < TALLER_TREN_RANURAS; pin++) {
        size_t largo = carga(b, pin, 1, esperas, niveles);
        assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == pin);
    }
    /* Refrescar el pin 0 lo hace el más reciente: el más antiguo pasa a ser el 1. */
    size_t largo = carga(b, 0, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == 0);
    largo = carga(b, 30, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == 1);
    assert(cancelada == -1 && s.ranuras[1].pin == 30);
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        assert(s.ranuras[i].activa);
    }
    /* Al terminar un tren, su ranura queda libre antes que sustituir otra. */
    s.ranuras[5].activa = false;
    largo = carga(b, 31, 1, esperas, niveles);
    assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) == 5);
}

static void reset_cancela_todo(void)
{
    TallerTren s;
    uint8_t b[TALLER_TREN_CARGA_MAX], pin, nivel;
    const uint32_t esperas[] = {10};
    const uint8_t niveles[] = {0};
    int cancelada;
    taller_tren_iniciar(&s);
    for (uint8_t p = 0; p < 3; p++) {
        size_t largo = carga(b, p, 1, esperas, niveles);
        assert(taller_tren_recibir(&s, b, largo, CLASICO, 0, &cancelada) >= 0);
    }
    taller_tren_reiniciar(&s);
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        assert(!taller_tren_avanzar(&s.ranuras[i], 1000, &pin, &nivel));
    }
    /* El plazo satura en lugar de desbordar. */
    assert(taller_tren_plazo(INT64_MAX - 5, 10) == INT64_MAX);
}

int main(void)
{
    pasos_relativos_y_esperas_cero();
    cargas_invalidas_se_ignoran_enteras();
    sustitucion_y_cancelacion();
    ocho_ranuras_y_novena();
    reset_cancela_todo();
    puts("tren: lógica OK");
    return 0;
}
