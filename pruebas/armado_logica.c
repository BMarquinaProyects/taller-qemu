/* Casos de comportamiento de la misma lógica C que usa el GPIO de QEMU. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_armado.h"

static const uint8_t dht[] = {
    4, 1, 0x00, 0x35, 0x0c, 0x00, 4, 2, 0, /* mínimo 800000 ns */
    0x30, 0x75, 0, 0, 0,                    /* 30000 ns -> 0 */
    0x80, 0x38, 1, 0, 1                     /* 80000 ns -> 1 */
};

static void flancos_y_pasos(void)
{
    TallerArmado s;
    uint8_t pin;
    bool nivel;
    taller_armado_iniciar(&s, 0);
    assert(taller_armado_recibir(&s, dht, sizeof(dht)));
    /* En entrada, OUT no es el nivel conducido: sigue alto. */
    assert(!taller_armado_observar(&s, 0, 0, 100));
    assert(!taller_armado_observar(&s, 0, 1ull << 4, 1000));
    assert(!taller_armado_observar(&s, 0, 0, 800999)); /* 799999: corto */
    assert(!taller_armado_observar(&s, 0, 1ull << 4, 900000));
    assert(taller_armado_observar(&s, 0, 0, 1700000) == 1); /* igual al mínimo */
    assert(s.ranuras[0].proximo_ns == 1730000);
    assert(!taller_armado_avanzar(&s.ranuras[0], 1729999, &pin, &nivel));
    assert(taller_armado_avanzar(&s.ranuras[0], 1730000, &pin, &nivel));
    assert(pin == 4 && !nivel && s.ranuras[0].proximo_ns == 1810000);
    /* OUT cambia estando en entrada: el tren externo no puede redisparar. */
    assert(!taller_armado_observar(&s, 1ull << 4, 0, 1740000));
    assert(taller_armado_avanzar(&s.ranuras[0], 1810000, &pin, &nivel));
    assert(pin == 4 && nivel && !s.ranuras[0].sonando);
    assert(!taller_armado_avanzar(&s.ranuras[0], 9999999, &pin, &nivel));
    assert(!taller_armado_observar(&s, 0, 1ull << 4, 2000000));
    assert(taller_armado_observar(&s, 0, 0, 2800001) == 1); /* persistente */
}

static void rearme_y_desarmado(void)
{
    TallerArmado s;
    uint8_t nuevo[sizeof(dht)], pin;
    bool nivel;
    memcpy(nuevo, dht, sizeof(dht));
    nuevo[6] = 18;
    nuevo[13] = 1; /* el tren nuevo empieza alto */
    taller_armado_iniciar(&s, 0);
    assert(taller_armado_recibir(&s, dht, sizeof(dht)));
    taller_armado_observar(&s, 0, 1ull << 4, 0);
    assert(taller_armado_observar(&s, 0, 0, 800000) == 1);
    assert(taller_armado_recibir(&s, nuevo, sizeof(nuevo)));
    /* Incluso un flanco válido se ignora mientras suena. */
    taller_armado_observar(&s, 0, 1ull << 4, 800000);
    assert(!taller_armado_observar(&s, 0, 0, 1600000));
    /* Callback tardío: el segundo plazo sigue anclado al flanco original. */
    assert(taller_armado_avanzar(&s.ranuras[0], 1600000, &pin, &nivel));
    assert(pin == 4 && !nivel && s.ranuras[0].proximo_ns == 910000);
    assert(taller_armado_avanzar(&s.ranuras[0], 1600000, &pin, &nivel));
    assert(pin == 4 && nivel);
    taller_armado_observar(&s, 0, 1ull << 4, 2000000);
    assert(taller_armado_observar(&s, 0, 0, 2800000) == 1);
    nuevo[7] = 0;
    assert(taller_armado_recibir(&s, nuevo, 9)); /* desarma, pero no corta */
    assert(taller_armado_avanzar(&s.ranuras[0], 2830000, &pin, &nivel));
    assert(pin == 18 && nivel);
    assert(taller_armado_avanzar(&s.ranuras[0], 2910000, &pin, &nivel));
    taller_armado_observar(&s, 0, 1ull << 4, 3000000);
    assert(!taller_armado_observar(&s, 0, 0, 3800000));
}

static void validacion_atomica(void)
{
    TallerArmado s, antes;
    uint8_t mal[1014] = {0};
    taller_armado_iniciar(&s, 0);
    assert(taller_armado_recibir(&s, dht, sizeof(dht)));
    antes = s;
    for (size_t n = 0; n < sizeof(dht); n++) {
        assert(!taller_armado_recibir(&s, dht, n));
        assert(!memcmp(&s, &antes, sizeof(s)));
    }
    /* Pin, flanco, pin de salida, n y nivel; todo el mensaje se ignora. */
    const int posiciones[] = {0, 1, 6, 7, 8, 18};
    const uint8_t valores[] = {49, 2, 49, 201, 1, 2};
    for (size_t i = 0; i < sizeof(posiciones) / sizeof(posiciones[0]); i++) {
        memcpy(mal, dht, sizeof(dht));
        mal[posiciones[i]] = valores[i];
        assert(!taller_armado_recibir(&s, mal, sizeof(dht)));
        assert(!memcmp(&s, &antes, sizeof(s)));
    }
    memcpy(mal, dht, sizeof(dht));
    assert(!taller_armado_recibir(&s, mal, sizeof(dht) + 1));
    memset(mal, 0, sizeof(mal));
    mal[0] = mal[6] = 39;
    mal[7] = 200;
    assert(taller_armado_recibir(&s, mal, 1009));
    /* 200 pasos de espera cero terminan, sin bucle infinito ni paso 201. */
    assert(taller_armado_observar(&s, 0, 1ull << 39, 0) == 2);
    for (int i = 0; i < 200; i++) {
        uint8_t pin;
        bool nivel;
        assert(taller_armado_avanzar(&s.ranuras[1], 0, &pin, &nivel));
        assert(pin == 39 && !nivel);
    }
    assert(!s.ranuras[1].sonando);
    mal[7] = 201;
    antes = s;
    assert(!taller_armado_recibir(&s, mal, sizeof(mal)));
    assert(!memcmp(&s, &antes, sizeof(s)));
}

static void ranuras_y_reloj(void)
{
    TallerArmado s;
    uint8_t carga[sizeof(dht)], pin;
    bool nivel;
    taller_armado_iniciar(&s, 0);
    memcpy(carga, dht, sizeof(dht));
    carga[1] = 0; /* bajada, edad inicial contada desde t=0 */
    for (int i = 0; i < 8; i++) {
        carga[0] = i;
        assert(taller_armado_recibir(&s, carga, sizeof(carga)));
    }
    carga[0] = 8;
    assert(!taller_armado_recibir(&s, carga, sizeof(carga)));
    assert(taller_armado_observar(&s, 0, 0xff, 800000) == 0xff);
    /* Una ranura desarmada pero aún sonando sigue ocupada. */
    carga[0] = 0;
    carga[7] = 0;
    assert(taller_armado_recibir(&s, carga, 9));
    carga[0] = 8;
    carga[7] = 2;
    assert(!taller_armado_recibir(&s, carga, sizeof(carga)));
    assert(taller_armado_avanzar(&s.ranuras[0], 830000, &pin, &nivel));
    assert(taller_armado_avanzar(&s.ranuras[0], 910000, &pin, &nivel));
    assert(taller_armado_recibir(&s, carga, sizeof(carga)));
    /* Reset: cancela ejecuciones y conserva armados; no inventa flancos. */
    taller_armado_reiniciar(&s, 1000000);
    assert(!taller_armado_observar(&s, 0, 0, 1000000));
    assert(!taller_armado_observar(&s, 0, 1ull << 8, 1799999));
    assert(!taller_armado_observar(&s, 0, 0, 1800000));
    assert(taller_armado_observar(&s, 0, 1ull << 8, 2600000) == 1);
    /* Suma de plazos saturada: nunca desborda un int64_t de QEMU. */
    taller_armado_reiniciar(&s, INT64_MAX - 1000000);
    assert(taller_armado_observar(&s, 0, 1ull << 8, INT64_MAX - 1) == 1);
    assert(s.ranuras[0].proximo_ns == INT64_MAX);
}

int main(void)
{
    flancos_y_pasos();
    rearme_y_desarmado();
    validacion_atomica();
    ranuras_y_reloj();
    puts("OK lógica C: flancos, pasos, rearme, validación, ranuras y reloj");
    return 0;
}
