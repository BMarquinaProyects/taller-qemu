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

static void plazos_del_dht_con_atencion_tardia(void)
{
    /* Mismo tren de seis pasos que inyecta el modo armado, desde t=1 ms. */
    static const int64_t previstos[] = {
        1030000, 1110000, 1190000, 1240000, 1310000, 1360000
    };
    static const int64_t atendidos[] = {
        1034000, 1112000, 1195000, 1241000, 1317000, 1369000
    };
    static const bool niveles[] = {false, true, false, true, false, true};
    static const uint8_t carga[] = {
        4, 1, 0x00, 0x35, 0x0c, 0, 4, 6, 0,
        0x30, 0x75, 0, 0, 0, /* 30 us */
        0x80, 0x38, 1, 0, 1, /* 80 us */
        0x80, 0x38, 1, 0, 0, /* 80 us */
        0x50, 0xc3, 0, 0, 1, /* 50 us */
        0x70, 0x11, 1, 0, 0, /* 70 us */
        0x50, 0xc3, 0, 0, 1  /* 50 us */
    };
    TallerArmado s;
    uint8_t pin;
    bool nivel;

    taller_armado_iniciar(&s, 0);
    assert(taller_armado_recibir(&s, carga, sizeof(carga)));
    assert(!taller_armado_observar(&s, 0, 1ull << 4, 0));
    assert(taller_armado_observar(&s, 0, 0, 1000000) == 1);
    TallerArmadoRanura *r = &s.ranuras[0];
    for (unsigned i = 0; i < 6; i++) {
        assert(r->proximo_ns == previstos[i]);
        assert(!taller_armado_avanzar(r, previstos[i] - 1, &pin, &nivel));
        assert(taller_armado_avanzar(r, atendidos[i], &pin, &nivel));
        assert(pin == 4 && nivel == niveles[i]);
    }
    assert(!r->sonando);
    assert(!taller_armado_avanzar(r, 2000000, &pin, &nivel));

    /* Una sola atención llega después de cuatro plazos: no desplaza el quinto. */
    assert(!taller_armado_observar(&s, 0, 1ull << 4, 2000000));
    assert(taller_armado_observar(&s, 0, 0, 3000000) == 1);
    for (unsigned i = 0; i < 4; i++) {
        assert(taller_armado_avanzar(r, 3245000, &pin, &nivel));
        assert(pin == 4 && nivel == niveles[i]);
    }
    assert(r->sonando && r->indice == 4 && r->proximo_ns == 3310000);
    assert(!taller_armado_avanzar(r, 3245000, &pin, &nivel));
    assert(taller_armado_avanzar(r, 3310000, &pin, &nivel));
    assert(!nivel && r->proximo_ns == 3360000);
    assert(taller_armado_avanzar(r, 3360000, &pin, &nivel));
    assert(nivel && !r->sonando);
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

static void alguno_sonando(void)
{
    TallerArmado s;
    uint8_t desarmado[9], pin;
    bool nivel;

    taller_armado_iniciar(&s, 0);
    assert(!taller_armado_alguno_sonando(&s));
    assert(taller_armado_recibir(&s, dht, sizeof(dht)));
    assert(!taller_armado_alguno_sonando(&s));
    taller_armado_observar(&s, 0, 1ull << 4, 0);
    assert(taller_armado_observar(&s, 0, 0, 800000) == 1);
    assert(taller_armado_alguno_sonando(&s));
    memcpy(desarmado, dht, sizeof(desarmado));
    desarmado[7] = 0;
    assert(taller_armado_recibir(&s, desarmado, sizeof(desarmado)));
    assert(!s.ranuras[0].armada && taller_armado_alguno_sonando(&s));
    assert(taller_armado_avanzar(&s.ranuras[0], 830000, &pin, &nivel));
    assert(taller_armado_alguno_sonando(&s));
    assert(taller_armado_avanzar(&s.ranuras[0], 910000, &pin, &nivel));
    assert(!taller_armado_alguno_sonando(&s));

    /* Recorre todas las ranuras, incluso las desarmadas que aún suenan. */
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        s.ranuras[i].sonando = true;
        assert(taller_armado_alguno_sonando(&s));
        s.ranuras[i].sonando = false;
        assert(!taller_armado_alguno_sonando(&s));
    }
    s.ranuras[0].sonando = s.ranuras[7].sonando = true;
    s.ranuras[0].sonando = false;
    assert(taller_armado_alguno_sonando(&s));
    taller_armado_reiniciar(&s, 1000000);
    assert(!taller_armado_alguno_sonando(&s));
}

static void condicion_del_cuanto_y_cola(void)
{
    TallerArmado s;
    taller_armado_iniciar(&s, 0);
    assert(!taller_armado_cuanto_activo(&s, 0));
    assert(taller_armado_recibir(&s, dht, sizeof(dht)));
    assert(!taller_armado_cuanto_activo(&s, INT64_C(5000000000)));
    s.fin_cola_ns = INT64_C(5000020000);
    assert(taller_armado_cuanto_activo(&s, INT64_C(5000019999)));
    assert(!taller_armado_cuanto_activo(&s, INT64_C(5000020000)));
    assert(!taller_armado_cuanto_activo(&s, INT64_C(5000020001)));
    /* Un tren activo mantiene el cuanto incluso con la cola anterior vencida. */
    s.ranuras[7].sonando = true;
    assert(taller_armado_cuanto_activo(&s, INT64_C(5000030000)));
    s.ranuras[7].sonando = false;
    s.fin_cola_ns = taller_armado_plazo(INT64_MAX - 10000,
                                       TALLER_ARMADO_CUANTO_COLA_NS);
    assert(s.fin_cola_ns == INT64_MAX);
    assert(taller_armado_cuanto_activo(&s, INT64_MAX - 1));
    assert(!taller_armado_cuanto_activo(&s, INT64_MAX));
    taller_armado_reiniciar(&s, INT64_C(5000010000));
    assert(s.fin_cola_ns == 0);
    assert(!taller_armado_cuanto_activo(&s, INT64_C(5000010000)));
    assert(s.ranuras[0].armada);
}

static void preparacion_del_cuanto(bool subida)
{
    uint8_t carga[] = {5, 0, 0x40, 0x1f, 0, 0, 19, 1, 0,
                       0x30, 0x75, 0, 0, 0}; /* mínimo 8 us; paso a +30 us. */
    const uint64_t bit = UINT64_C(1) << 5;
    const uint64_t reposo = subida ? bit : 0;
    const uint64_t preparacion = subida ? 0 : bit;
    const int64_t base_ns = INT64_C(5000000000);
    TallerArmado s, antes;
    uint8_t pin;
    bool nivel;

    carga[1] = subida;
    taller_armado_iniciar(&s, base_ns);
    assert(!taller_armado_observar(&s, preparacion, bit, base_ns));
    assert(!taller_armado_cuanto_activo(&s, base_ns)); /* Sin armar no cuenta. */
    assert(taller_armado_recibir(&s, carga, sizeof(carga)));
    assert(taller_armado_cuanto_activo(&s, base_ns));
    antes = s;
    assert(!taller_armado_cuanto_activo(&s, base_ns - 1));
    assert(taller_armado_cuanto_activo(&s, base_ns + 27999));
    assert(!taller_armado_cuanto_activo(&s, base_ns + 28000));
    assert(!taller_armado_cuanto_activo(&s, base_ns + 28001));
    assert(!memcmp(&s, &antes, sizeof(s))); /* Consulta pura. */

    /* Volver a reposo tras solo 5 us cancela preparación, sin disparar. */
    assert(!taller_armado_observar(&s, reposo, bit, base_ns + 5000));
    assert(!taller_armado_cuanto_activo(&s, base_ns + 5000));
    s.fin_cola_ns = base_ns + 20000;
    assert(taller_armado_cuanto_activo(&s, base_ns + 5000));
    assert(!taller_armado_cuanto_activo(&s, base_ns + 20000));

    /* El mínimo inclusivo enlaza preparación con tren sin perder actividad. */
    assert(!taller_armado_observar(&s, preparacion, bit, base_ns + 100000));
    assert(taller_armado_cuanto_activo(&s, base_ns + 107999));
    assert(taller_armado_observar(&s, reposo, bit, base_ns + 108000) == 1);
    assert(s.ranuras[0].sonando);
    assert(taller_armado_cuanto_activo(&s, base_ns + 108000));
    /* Durante el tren, cambiar el pin no prepara otro disparo. */
    assert(!taller_armado_observar(&s, preparacion, bit, base_ns + 109000));
    assert(!taller_armado_observar(&s, reposo, bit, base_ns + 110000));
    assert(taller_armado_avanzar(&s.ranuras[0], base_ns + 138000, &pin, &nivel));
    assert(!taller_armado_cuanto_activo(&s, base_ns + 138000));
    /* Desarmar durante preparación apaga el cuanto. */
    assert(!taller_armado_observar(&s, preparacion, bit, base_ns + 140000));
    carga[7] = 0;
    assert(taller_armado_recibir(&s, carga, 9));
    assert(!taller_armado_cuanto_activo(&s, base_ns + 140000));
    taller_armado_reiniciar(&s, base_ns + 141000);
    assert(!taller_armado_cuanto_activo(&s, base_ns + 141000));
}

static void margen_de_preparacion_de_64_bits(void)
{
    uint8_t carga[] = {48, 1, 0xff, 0xff, 0xff, 0xff, 47, 1, 0,
                       0x30, 0x75, 0, 0, 0};
    const int64_t base_ns = INT64_C(5000000000);
    TallerArmado s;

    taller_armado_iniciar(&s, base_ns);
    assert(taller_armado_recibir(&s, carga, sizeof(carga)));
    assert(!taller_armado_observar(&s, 0, UINT64_C(1) << 48, base_ns));
    /* 0xffffffff + 20000 = 4294987295; la suma no cabe en 32 bits. */
    assert(taller_armado_cuanto_activo(&s, base_ns + INT64_C(4294987294)));
    assert(!taller_armado_cuanto_activo(&s, base_ns + INT64_C(4294987295)));
    taller_armado_reiniciar(&s, INT64_MAX - 100000);
    assert(!taller_armado_observar(&s, 0, UINT64_C(1) << 48,
                                   INT64_MAX - 100000));
    assert(taller_armado_cuanto_activo(&s, INT64_MAX));
    taller_armado_reiniciar(&s, INT64_MAX);
    assert(s.ranuras[0].armada);
    assert(!taller_armado_cuanto_activo(&s, INT64_MAX));
}

int main(void)
{
    flancos_y_pasos();
    rearme_y_desarmado();
    plazos_del_dht_con_atencion_tardia();
    validacion_atomica();
    ranuras_y_reloj();
    alguno_sonando();
    condicion_del_cuanto_y_cola();
    preparacion_del_cuanto(false);
    preparacion_del_cuanto(true);
    margen_de_preparacion_de_64_bits();
    puts("OK lógica C: flancos, pasos, rearme, validación, reloj, preparación y cola");
    return 0;
}
