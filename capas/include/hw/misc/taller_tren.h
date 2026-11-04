/*
 * ENTRADA_TREN (puente v8): secuencias inmediatas de niveles 0/1/2 en tiempo
 * virtual, sin esperar un flanco del firmware (rebote de contactos, hueco de un
 * conmutador «break-before-make»). Lógica pura, sin dependencias de QEMU: el
 * llamante aporta el reloj, programa un temporizador por ranura y aplica cada
 * nivel devuelto por la misma ruta que ENTRADA.
 *
 * Carga: u8 pin, u16 n, n × (u32 espera_ns, u8 nivel); LE, 3 + 5·n bytes.
 *   - n de 0 a 64; n = 0 solo cancela el tren pendiente del pin.
 *   - nivel 0, 1 o 2 (2 = suelto); un pin fuera del chip invalida la carga.
 *   - Una carga inválida se ignora entera y no cancela nada.
 *   - Primera espera desde la recepción; las demás, desde el paso anterior.
 *   - Un tren nuevo en el mismo pin sustituye al pendiente.
 *   - Ocho ranuras; un noveno pin sustituye al tren más antiguo.
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#ifndef HW_MISC_TALLER_TREN_H
#define HW_MISC_TALLER_TREN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TALLER_TREN_RANURAS 8
#define TALLER_TREN_PASOS 64
#define TALLER_TREN_CARGA_MAX (3 + 5 * TALLER_TREN_PASOS)

typedef struct TallerTrenPaso {
    uint32_t espera_ns;
    uint8_t nivel;
} TallerTrenPaso;

typedef struct TallerTrenRanura {
    bool activa;
    uint8_t pin;
    uint16_t n;
    uint16_t indice;
    int64_t proximo_ns;
    uint64_t orden; /* para sustituir al más antiguo */
    TallerTrenPaso pasos[TALLER_TREN_PASOS];
} TallerTrenRanura;

typedef struct TallerTren {
    uint64_t contador;
    TallerTrenRanura ranuras[TALLER_TREN_RANURAS];
} TallerTren;

static inline void taller_tren_iniciar(TallerTren *s)
{
    memset(s, 0, sizeof(*s));
}

/* Reset del chip: los trenes pendientes se cancelan. */
static inline void taller_tren_reiniciar(TallerTren *s)
{
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        s->ranuras[i].activa = false;
    }
}

static inline int64_t taller_tren_plazo(int64_t base_ns, uint32_t espera_ns)
{
    return base_ns > INT64_MAX - (int64_t)espera_ns ? INT64_MAX
                                                    : base_ns + espera_ns;
}

/* Devuelve la ranura del tren pendiente del pin (y la libera) o -1. */
static inline int taller_tren_cancelar(TallerTren *s, uint8_t pin)
{
    for (unsigned i = 0; i < TALLER_TREN_RANURAS; i++) {
        if (s->ranuras[i].activa && s->ranuras[i].pin == pin) {
            s->ranuras[i].activa = false;
            return (int)i;
        }
    }
    return -1;
}

/*
 * Valida la carga entera y la instala. `validos` = bit n si el GPIO n existe.
 * Devuelve la ranura que hay que (re)programar, -1 si la carga es inválida o
 * -2 si era una cancelación (n = 0). *cancelada recibe la ranura liberada
 * (-1 si no había tren), para que el llamante pare su temporizador.
 */
static inline int taller_tren_recibir(TallerTren *s, const uint8_t *datos,
                                      size_t largo, uint64_t validos,
                                      int64_t ahora_ns, int *cancelada)
{
    TallerTrenRanura *destino = NULL;
    uint16_t n;
    uint8_t pin;

    *cancelada = -1;
    if (largo < 3) {
        return -1;
    }
    pin = datos[0];
    n = (uint16_t)datos[1] | ((uint16_t)datos[2] << 8);
    if (pin >= 64 || !(validos & (UINT64_C(1) << pin)) ||
        n > TALLER_TREN_PASOS || largo != 3 + 5 * (size_t)n) {
        return -1;
    }
    for (unsigned i = 0; i < n; i++) {
        if (datos[3 + 5 * i + 4] > 2) {
            return -1;
        }
    }
    *cancelada = taller_tren_cancelar(s, pin);
    if (!n) {
        return -2;
    }
    if (*cancelada >= 0) {
        destino = &s->ranuras[*cancelada];
    }
    for (unsigned i = 0; !destino && i < TALLER_TREN_RANURAS; i++) {
        if (!s->ranuras[i].activa) {
            destino = &s->ranuras[i];
        }
    }
    if (!destino) {
        destino = &s->ranuras[0];
        for (unsigned i = 1; i < TALLER_TREN_RANURAS; i++) {
            if (s->ranuras[i].orden < destino->orden) {
                destino = &s->ranuras[i];
            }
        }
    }
    destino->activa = true;
    destino->pin = pin;
    destino->n = n;
    destino->indice = 0;
    destino->orden = ++s->contador;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *paso = datos + 3 + 5 * i;

        destino->pasos[i].espera_ns = (uint32_t)paso[0] |
                                      ((uint32_t)paso[1] << 8) |
                                      ((uint32_t)paso[2] << 16) |
                                      ((uint32_t)paso[3] << 24);
        destino->pasos[i].nivel = paso[4];
    }
    destino->proximo_ns = taller_tren_plazo(ahora_ns, destino->pasos[0].espera_ns);
    return (int)(destino - s->ranuras);
}

/* Extrae un paso vencido; los plazos son acumulativos y no arrastran latencia. */
static inline bool taller_tren_avanzar(TallerTrenRanura *r, int64_t ahora_ns,
                                       uint8_t *pin, uint8_t *nivel)
{
    if (!r->activa || ahora_ns < r->proximo_ns) {
        return false;
    }
    *pin = r->pin;
    *nivel = r->pasos[r->indice].nivel;
    if (++r->indice == r->n) {
        r->activa = false;
    } else {
        r->proximo_ns = taller_tren_plazo(r->proximo_ns,
                                          r->pasos[r->indice].espera_ns);
    }
    return true;
}

#endif /* HW_MISC_TALLER_TREN_H */
