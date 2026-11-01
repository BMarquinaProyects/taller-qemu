/*
 * Trenes armados: decisiones deterministas, sin dependencias de QEMU ni E/S.
 * El llamante aporta el reloj y aplica los pasos devueltos a la entrada externa.
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#ifndef HW_MISC_TALLER_ARMADO_H
#define HW_MISC_TALLER_ARMADO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TALLER_ARMADO_PINES 40
#define TALLER_ARMADO_RANURAS 8
#define TALLER_ARMADO_PASOS 200
#define TALLER_ARMADO_CARGA_MAX (9 + 5 * TALLER_ARMADO_PASOS)
#define TALLER_ARMADO_MASCARA ((1ull << TALLER_ARMADO_PINES) - 1)

typedef struct TallerArmadoPaso {
    uint32_t espera_ns;
    bool nivel;
} TallerArmadoPaso;

typedef struct TallerArmadoPrograma {
    uint8_t pin_disparo;
    bool subida;
    uint32_t minimo_ns;
    uint8_t pin_salida;
    uint16_t n;
    TallerArmadoPaso pasos[TALLER_ARMADO_PASOS];
} TallerArmadoPrograma;

typedef struct TallerArmadoRanura {
    bool armada;
    bool sonando;
    TallerArmadoPrograma programa;
    /* Copia inmutable durante la reproducción: rearmar no la modifica. */
    TallerArmadoPrograma en_curso;
    uint16_t indice;
    int64_t proximo_ns;
} TallerArmadoRanura;

typedef struct TallerArmado {
    uint64_t conducido;
    int64_t cambio_ns[TALLER_ARMADO_PINES];
    TallerArmadoRanura ranuras[TALLER_ARMADO_RANURAS];
} TallerArmado;

static inline uint32_t taller_armado_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Solo publica el programa si TODA la carga es válida, sin relleno ni sobrantes. */
static inline bool taller_armado_decodificar(const uint8_t *datos, size_t largo,
                                            TallerArmadoPrograma *programa)
{
    TallerArmadoPrograma p = {0};

    if (largo < 9) {
        return false;
    }
    p.n = (uint16_t)datos[7] | ((uint16_t)datos[8] << 8);
    if (datos[0] >= TALLER_ARMADO_PINES || datos[1] > 1 ||
        datos[6] >= TALLER_ARMADO_PINES || p.n > TALLER_ARMADO_PASOS ||
        largo != 9 + 5 * (size_t)p.n) {
        return false;
    }
    p.pin_disparo = datos[0];
    p.subida = datos[1];
    p.minimo_ns = taller_armado_u32(datos + 2);
    p.pin_salida = datos[6];
    for (unsigned i = 0; i < p.n; i++) {
        const uint8_t *paso = datos + 9 + 5 * i;

        if (paso[4] > 1) {
            return false;
        }
        p.pasos[i].espera_ns = taller_armado_u32(paso);
        p.pasos[i].nivel = paso[4];
    }
    *programa = p;
    return true;
}

/* Un desarmado conserva su ranura mientras termina el tren que ya suena. */
static inline bool taller_armado_recibir(TallerArmado *s, const uint8_t *datos,
                                        size_t largo)
{
    TallerArmadoPrograma p;
    TallerArmadoRanura *libre = NULL;

    if (!taller_armado_decodificar(datos, largo, &p)) {
        return false;
    }
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        TallerArmadoRanura *r = &s->ranuras[i];

        if ((r->armada || r->sonando) &&
            r->programa.pin_disparo == p.pin_disparo) {
            r->programa = p;
            r->armada = p.n != 0;
            return true;
        }
        if (!r->armada && !r->sonando && !libre) {
            libre = r;
        }
    }
    if (!p.n) {
        return true; /* desarmar un pin inexistente no ocupa una ranura */
    }
    if (!libre) {
        return false;
    }
    libre->programa = p;
    libre->armada = true;
    return true;
}

static inline void taller_armado_reiniciar(TallerArmado *s, int64_t ahora_ns)
{
    s->conducido = TALLER_ARMADO_MASCARA;
    for (unsigned pin = 0; pin < TALLER_ARMADO_PINES; pin++) {
        s->cambio_ns[pin] = ahora_ns;
    }
    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        s->ranuras[i].sonando = false;
    }
}

static inline void taller_armado_iniciar(TallerArmado *s, int64_t ahora_ns)
{
    memset(s, 0, sizeof(*s));
    taller_armado_reiniciar(s, ahora_ns);
}

static inline int64_t taller_armado_plazo(int64_t base_ns, uint32_t espera_ns)
{
    return base_ns > INT64_MAX - espera_ns ? INT64_MAX : base_ns + espera_ns;
}

/* Devuelve una máscara de ranuras que acaban de empezar (un DISPARO por bit). */
static inline uint8_t taller_armado_observar(TallerArmado *s, uint64_t out,
                                           uint64_t enable, int64_t ahora_ns)
{
    /*
     * Se vigila lo CONDUCIDO por el ESP32: enable ? out : 1. En entrada el
     * pull-up del módulo cuenta como alto. Usar IN/in_externo permitiría que
     * los pulsos del propio sensor volvieran a disparar su tren (DHT).
     */
    uint64_t nuevo = ((out & enable) | ~enable) & TALLER_ARMADO_MASCARA;
    uint64_t cambios = nuevo ^ s->conducido;
    uint8_t disparos = 0;

    for (unsigned i = 0; i < TALLER_ARMADO_RANURAS; i++) {
        TallerArmadoRanura *r = &s->ranuras[i];
        const TallerArmadoPrograma *p = &r->programa;
        uint64_t bit = 1ull << p->pin_disparo;

        if (!r->armada || r->sonando || !(cambios & bit) ||
            !!(nuevo & bit) != p->subida ||
            ahora_ns < s->cambio_ns[p->pin_disparo] ||
            (uint64_t)(ahora_ns - s->cambio_ns[p->pin_disparo]) < p->minimo_ns) {
            continue;
        }
        r->en_curso = *p;
        r->sonando = true;
        r->indice = 0;
        r->proximo_ns = taller_armado_plazo(ahora_ns, p->pasos[0].espera_ns);
        disparos |= 1u << i;
    }
    for (unsigned pin = 0; pin < TALLER_ARMADO_PINES; pin++) {
        if (cambios & (1ull << pin)) {
            s->cambio_ns[pin] = ahora_ns;
        }
    }
    s->conducido = nuevo;
    return disparos;
}

/* Extrae un paso vencido. Los plazos son acumulativos: no arrastran latencia. */
static inline bool taller_armado_avanzar(TallerArmadoRanura *r, int64_t ahora_ns,
                                        uint8_t *pin, bool *nivel)
{
    if (!r->sonando || ahora_ns < r->proximo_ns) {
        return false;
    }
    *pin = r->en_curso.pin_salida;
    *nivel = r->en_curso.pasos[r->indice].nivel;
    if (++r->indice == r->en_curso.n) {
        r->sonando = false;
    } else {
        r->proximo_ns = taller_armado_plazo(r->proximo_ns,
                                          r->en_curso.pasos[r->indice].espera_ns);
    }
    return true;
}

#endif /* HW_MISC_TALLER_ARMADO_H */
