/*
 * 1-Wire (M-244 de Taller): sensores DS18B20 que contestan dentro de QEMU, con el reloj virtual.
 * Lógica pura, sin dependencias de QEMU ni E/S: el GPIO le cuenta cada cambio de lo que conduce
 * el ESP32 en el pin del bus (enable ? out : 1) y aplica los pulsos bajos que devuelve, por la
 * misma ruta que ENTRADA_TREN. El puente (trama 22 UNHILO) solo trae la ROM y la temperatura de
 * cada sensor: las ranuras de 1-Wire duran 60 µs y no hay tiempo para preguntar fuera.
 *
 * Hoja de Maxim (DS18B20, Rev 6): reset ≥ 480 µs y presencia de 60–240 µs tras 15–60 µs; ranuras
 * de 60–120 µs, el maestro escribe un 1 con 1–15 µs en bajo y un 0 con 60–120; en las de lectura
 * el sensor mantiene la línea baja para un 0 (el maestro muestrea antes de 15 µs). LSB primero.
 * ROM: familia 28h, 48 bits de serie y CRC8 (X^8+X^5+X^4+1). Scratchpad: T (1/16 °C, LSB y MSB),
 * TH, TL, configuración (0 R1 R0 1 1 1 1 1), FFh, reservado, 10h y CRC. Al encender, T = +85 °C.
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#ifndef HW_MISC_TALLER_UNHILO_H
#define HW_MISC_TALLER_UNHILO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TALLER_UNHILO_BUSES 4
#define TALLER_UNHILO_SENSORES 8
#define TALLER_UNHILO_CARGA 12
#define TALLER_UNHILO_RESET_NS INT64_C(400000)  /* la hoja pide 480 µs; se acepta desde 400 */
#define TALLER_UNHILO_UNO_NS INT64_C(15000)     /* más corto: el maestro escribe un 1 */
#define TALLER_UNHILO_PRESENCIA_ESPERA_NS 30000u
#define TALLER_UNHILO_PRESENCIA_NS 120000u
#define TALLER_UNHILO_CERO_NS 30000u            /* lo que el sensor sujeta la línea en un 0 */
#define TALLER_UNHILO_EEPROM_NS INT64_C(10000000)
/* Tras un cambio en el bus, cuánto dura el cuanto corto de las dos CPU (M-157): una ranura de 6 µs
 * no puede alargarse a más de 15 µs porque la otra CPU tenga el turno. */
#define TALLER_UNHILO_COLA_NS INT64_C(1000000)

typedef enum TallerUnHiloFase {
    TALLER_UNHILO_ESPERA,      /* sin reset: no hace caso */
    TALLER_UNHILO_ROM,         /* recibe el comando de ROM */
    TALLER_UNHILO_MATCH,       /* recibe los 64 bits de Match ROM */
    TALLER_UNHILO_BUSQUEDA,    /* Search ROM: bit, complemento y dirección */
    TALLER_UNHILO_FUNCION,     /* recibe el comando de función */
    TALLER_UNHILO_ESCRIBIR,    /* Write Scratchpad: TH, TL y configuración */
    TALLER_UNHILO_ENVIAR,      /* Read ROM, Read Scratchpad */
    TALLER_UNHILO_ESTADO,      /* Convert T, Copy, Recall, Read Power Supply: ranuras de estado */
} TallerUnHiloFase;

typedef struct TallerUnHiloSensor {
    bool usado;
    uint8_t rom[8];
    int16_t medida;            /* lo que marca el mando, en 1/16 °C */
    uint8_t sp[9];
    uint8_t eeprom[3];         /* TH, TL y configuración */
    bool elegido;              /* tras Skip/Match/Search */
    bool participa;            /* sigue en la búsqueda */
    int64_t ocupado_hasta;
    int64_t convertir_hasta;   /* fin de la conversión en curso (0 = ninguna) */
} TallerUnHiloSensor;

typedef struct TallerUnHiloBus {
    bool activo;
    uint8_t pin;
    bool conducido;            /* lo último que conduce el ESP32: 1 = suelta */
    int64_t ultimo_cambio_ns;
    int64_t bajada_ns;
    TallerUnHiloFase fase;
    uint8_t comando;
    uint8_t byte;
    unsigned bits;             /* bits recibidos o enviados en la fase */
    unsigned total;            /* bits a enviar en ENVIAR */
    uint8_t busqueda_paso;     /* 0 bit, 1 complemento, 2 dirección */
    bool lectura;              /* la ranura en curso la pone el sensor: su subida no es un bit */
    uint8_t rom_match[8];
    TallerUnHiloSensor s[TALLER_UNHILO_SENSORES];
} TallerUnHiloBus;

typedef struct TallerUnHilo {
    TallerUnHiloBus buses[TALLER_UNHILO_BUSES];
} TallerUnHilo;

/* Un pulso bajo que imponer en el pin: empieza `espera_ns` después del cambio y dura `duracion_ns`. */
typedef struct TallerUnHiloPulso {
    uint8_t pin;
    uint32_t espera_ns;
    uint32_t duracion_ns;
} TallerUnHiloPulso;

static inline uint8_t taller_unhilo_crc8(const uint8_t *datos, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = datos[i];
        for (unsigned j = 0; j < 8; j++) {
            uint8_t mezcla = (crc ^ b) & 1;
            crc >>= 1;
            if (mezcla) {
                crc ^= 0x8C; /* X^8 + X^5 + X^4 + 1, reflejado */
            }
            b >>= 1;
        }
    }
    return crc;
}

static inline void taller_unhilo_iniciar(TallerUnHilo *u)
{
    memset(u, 0, sizeof(*u));
}

/* Reset del ESP32: los sensores siguen alimentados (conservan su scratchpad); el bus vuelve a
 * esperar un reset de 1-Wire. */
static inline void taller_unhilo_reiniciar(TallerUnHilo *u)
{
    for (unsigned i = 0; i < TALLER_UNHILO_BUSES; i++) {
        TallerUnHiloBus *b = &u->buses[i];
        b->conducido = true;
        b->lectura = false;
        b->fase = TALLER_UNHILO_ESPERA;
        b->bits = 0;
        for (unsigned j = 0; j < TALLER_UNHILO_SENSORES; j++) {
            b->s[j].elegido = b->s[j].participa = false;
        }
    }
}

static inline void taller_unhilo_sellar(TallerUnHiloSensor *s)
{
    s->sp[8] = taller_unhilo_crc8(s->sp, 8);
}

/* Al encender: +85 °C y TH, TL y configuración desde la EEPROM (de fábrica 75, 70 y 12 bits). */
static inline void taller_unhilo_encender(TallerUnHiloSensor *s)
{
    const uint8_t sp[8] = {0x50, 0x05, s->eeprom[0], s->eeprom[1], s->eeprom[2], 0xFF, 0x0C, 0x10};
    memcpy(s->sp, sp, 8);
    taller_unhilo_sellar(s);
    s->elegido = s->participa = false;
    s->ocupado_hasta = s->convertir_hasta = 0;
}

/* Tiempo de conversión: el 80 % del máximo de la hoja (93,75 ms · 2^(bits − 9)), como el real. */
static inline int64_t taller_unhilo_conversion_ns(const TallerUnHiloSensor *s)
{
    unsigned r = (s->sp[4] >> 5) & 3;
    return (INT64_C(75000000) << r);
}

/* Acaba una conversión: la medida, redondeada a la resolución (los bits sobrantes, a 0). */
static inline void taller_unhilo_terminar(TallerUnHiloSensor *s, int64_t ahora_ns)
{
    if (!s->convertir_hasta || ahora_ns < s->convertir_hasta) {
        return;
    }
    unsigned r = (s->sp[4] >> 5) & 3;
    uint16_t t = (uint16_t)s->medida & (uint16_t)~((1u << (3 - r)) - 1u);
    s->sp[0] = (uint8_t)t;
    s->sp[1] = (uint8_t)(t >> 8);
    taller_unhilo_sellar(s);
    s->convertir_hasta = 0;
}

static inline TallerUnHiloBus *taller_unhilo_bus(TallerUnHilo *u, uint8_t pin, bool crear)
{
    TallerUnHiloBus *libre = NULL;
    for (unsigned i = 0; i < TALLER_UNHILO_BUSES; i++) {
        if (u->buses[i].activo && u->buses[i].pin == pin) {
            return &u->buses[i];
        }
        if (!u->buses[i].activo && !libre) {
            libre = &u->buses[i];
        }
    }
    if (!crear || !libre) {
        return NULL;
    }
    memset(libre, 0, sizeof(*libre));
    libre->activo = true;
    libre->pin = pin;
    libre->conducido = true;
    return libre;
}

/*
 * Trama 22 UNHILO: u8 pin, u8 rom[8], i16 medida (1/16 °C, LE), u8 bits (bit 0 = presente).
 * Da de alta o actualiza el sensor de esa ROM en ese pin; con el bit 0 a 0, lo quita (y el bus
 * queda libre si ya no tiene ninguno). Devuelve false si la carga no vale.
 */
static inline bool taller_unhilo_recibir(TallerUnHilo *u, const uint8_t *d, size_t largo,
                                         uint64_t mascara)
{
    if (largo != TALLER_UNHILO_CARGA || d[0] >= 64 || !(mascara & (1ull << d[0]))) {
        return false;
    }
    bool presente = d[11] & 1;
    TallerUnHiloBus *b = taller_unhilo_bus(u, d[0], presente);
    if (!b) {
        return !presente;
    }
    TallerUnHiloSensor *hueco = NULL;
    for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
        TallerUnHiloSensor *s = &b->s[i];
        if (s->usado && !memcmp(s->rom, d + 1, 8)) {
            hueco = s;
            break;
        }
        if (!s->usado && !hueco) {
            hueco = s;
        }
    }
    if (!hueco) {
        return false;
    }
    if (!presente) {
        hueco->usado = false;
        bool queda = false;
        for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
            queda |= b->s[i].usado;
        }
        b->activo = queda;
        return true;
    }
    if (!hueco->usado) {
        hueco->usado = true;
        memcpy(hueco->rom, d + 1, 8);
        hueco->eeprom[0] = 0x4B;
        hueco->eeprom[1] = 0x46;
        hueco->eeprom[2] = 0x7F;
        taller_unhilo_encender(hueco);
    }
    hueco->medida = (int16_t)(uint16_t)(d[9] | (d[10] << 8));
    return true;
}

/* Bit que pone el sensor en la ranura `i` de lo que envía (ROM o scratchpad, LSB primero). */
static inline bool taller_unhilo_bit_envio(const TallerUnHiloBus *b, const TallerUnHiloSensor *s,
                                           unsigned i)
{
    const uint8_t *fuente = b->comando == 0x33 ? s->rom : s->sp;
    return (fuente[i / 8] >> (i % 8)) & 1;
}

/* Bit de la ranura de estado: 0 mientras trabaja (Convert, Copy, Recall); en Read Power Supply, 1. */
static inline bool taller_unhilo_bit_estado(const TallerUnHiloSensor *s, int64_t ahora_ns)
{
    return ahora_ns >= s->ocupado_hasta && (!s->convertir_hasta || ahora_ns >= s->convertir_hasta);
}

/* Lo que pone la línea un sensor en la ranura que empieza ahora: AND cableado de todos. */
static inline bool taller_unhilo_linea(TallerUnHiloBus *b, int64_t ahora_ns)
{
    bool linea = true;
    for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
        TallerUnHiloSensor *s = &b->s[i];
        if (!s->usado) {
            continue;
        }
        taller_unhilo_terminar(s, ahora_ns);
        switch (b->fase) {
        case TALLER_UNHILO_ENVIAR:
            if (s->elegido || b->comando == 0x33) {
                linea &= taller_unhilo_bit_envio(b, s, b->bits);
            }
            break;
        case TALLER_UNHILO_ESTADO:
            if (s->elegido) {
                linea &= taller_unhilo_bit_estado(s, ahora_ns);
            }
            break;
        case TALLER_UNHILO_BUSQUEDA:
            if (s->participa && b->busqueda_paso < 2) {
                bool bit = (s->rom[b->bits / 8] >> (b->bits % 8)) & 1;
                linea &= b->busqueda_paso == 0 ? bit : !bit;
            }
            break;
        default:
            break;
        }
    }
    return linea;
}

static inline bool taller_unhilo_alarma(const TallerUnHiloSensor *s)
{
    int grados = (int16_t)(uint16_t)(s->sp[0] | (s->sp[1] << 8)) >> 4;
    return grados >= (int8_t)s->sp[2] || grados <= (int8_t)s->sp[3];
}

/* Un comando de función recibido entero. */
static inline void taller_unhilo_funcion(TallerUnHiloBus *b, uint8_t comando, int64_t ahora_ns)
{
    b->comando = comando;
    b->bits = 0;
    b->byte = 0;
    b->fase = TALLER_UNHILO_ESPERA;
    for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
        TallerUnHiloSensor *s = &b->s[i];
        if (!s->usado || !s->elegido) {
            continue;
        }
        taller_unhilo_terminar(s, ahora_ns);
        switch (comando) {
        case 0x44:
            s->convertir_hasta = ahora_ns + taller_unhilo_conversion_ns(s);
            b->fase = TALLER_UNHILO_ESTADO;
            break;
        case 0xBE:
            b->fase = TALLER_UNHILO_ENVIAR;
            b->total = 72;
            break;
        case 0x4E:
            b->fase = TALLER_UNHILO_ESCRIBIR;
            break;
        case 0x48:
            memcpy(s->eeprom, s->sp + 2, 3);
            s->ocupado_hasta = ahora_ns + TALLER_UNHILO_EEPROM_NS;
            b->fase = TALLER_UNHILO_ESTADO;
            break;
        case 0xB8:
            memcpy(s->sp + 2, s->eeprom, 3);
            taller_unhilo_sellar(s);
            b->fase = TALLER_UNHILO_ESTADO;
            break;
        case 0xB4:
            b->fase = TALLER_UNHILO_ESTADO; /* alimentado por VDD: ranuras a 1 */
            break;
        default:
            break;
        }
    }
}

/* Un bit escrito por el maestro. */
static inline void taller_unhilo_bit_recibido(TallerUnHiloBus *b, bool bit, int64_t ahora_ns)
{
    switch (b->fase) {
    case TALLER_UNHILO_BUSQUEDA:
        if (b->busqueda_paso != 2) {
            return;
        }
        {
            for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
                TallerUnHiloSensor *s = &b->s[i];
                if (s->usado && s->participa && (((s->rom[b->bits / 8] >> (b->bits % 8)) & 1) != bit)) {
                    s->participa = false;
                }
            }
            if (++b->bits == 64) {
                for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
                    b->s[i].elegido = b->s[i].usado && b->s[i].participa;
                }
                b->fase = TALLER_UNHILO_FUNCION;
                b->bits = 0;
                b->byte = 0;
                b->busqueda_paso = 0;
                return;
            }
        }
        b->busqueda_paso = 0;
        return;
    case TALLER_UNHILO_ROM:
    case TALLER_UNHILO_MATCH:
    case TALLER_UNHILO_FUNCION:
    case TALLER_UNHILO_ESCRIBIR:
        break;
    default:
        return; /* en las ranuras de lectura, el bit del maestro no cuenta */
    }
    b->byte = (uint8_t)((b->byte >> 1) | (bit ? 0x80 : 0));
    b->bits++;
    if (b->fase == TALLER_UNHILO_MATCH) {
        if (b->bits % 8 == 0) {
            b->rom_match[b->bits / 8 - 1] = b->byte;
        }
        if (b->bits == 64) {
            for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
                TallerUnHiloSensor *s = &b->s[i];
                s->elegido = s->usado && !memcmp(s->rom, b->rom_match, 8);
            }
            b->fase = TALLER_UNHILO_FUNCION;
            b->bits = 0;
        }
        return;
    }
    if (b->bits % 8) {
        return;
    }
    uint8_t byte = b->byte;
    if (b->fase == TALLER_UNHILO_ESCRIBIR) {
        unsigned k = b->bits / 8 - 1;
        for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
            TallerUnHiloSensor *s = &b->s[i];
            if (s->usado && s->elegido) {
                s->sp[2 + k] = k == 2 ? (uint8_t)((byte & 0x60) | 0x1F) : byte;
                taller_unhilo_sellar(s);
            }
        }
        if (k == 2) {
            b->fase = TALLER_UNHILO_ESPERA;
        }
        return;
    }
    b->bits = 0;
    if (b->fase == TALLER_UNHILO_FUNCION) {
        taller_unhilo_funcion(b, byte, ahora_ns);
        return;
    }
    /* Comando de ROM. */
    b->comando = byte;
    for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
        TallerUnHiloSensor *s = &b->s[i];
        taller_unhilo_terminar(s, ahora_ns);
        s->elegido = s->usado && byte == 0xCC;
        s->participa = s->usado && (byte == 0xF0 || (byte == 0xEC && taller_unhilo_alarma(s)));
    }
    switch (byte) {
    case 0x33:
        b->fase = TALLER_UNHILO_ENVIAR;
        b->total = 64;
        break;
    case 0x55:
        b->fase = TALLER_UNHILO_MATCH;
        break;
    case 0xCC:
        b->fase = TALLER_UNHILO_FUNCION;
        break;
    case 0xF0:
    case 0xEC:
        b->fase = TALLER_UNHILO_BUSQUEDA;
        b->busqueda_paso = 0;
        break;
    default:
        b->fase = TALLER_UNHILO_ESPERA;
        break;
    }
}

/*
 * Un cambio de lo que conduce el ESP32 en el pin del bus. Con un pulso que imponer, rellena
 * `pulso` y devuelve true. En una bajada empieza la ranura: si algún sensor pone un 0 (o hay
 * que leer un 0 de estado), sujeta la línea desde ese mismo instante. En una subida, la
 * duración del nivel bajo dice si era un reset (y toca presencia) o un bit escrito.
 */
static inline bool taller_unhilo_cambio(TallerUnHiloBus *b, bool nivel, int64_t ahora_ns,
                                        TallerUnHiloPulso *pulso)
{
    if (nivel == b->conducido) {
        return false;
    }
    b->conducido = nivel;
    b->ultimo_cambio_ns = ahora_ns;
    pulso->pin = b->pin;
    if (!nivel) {
        b->bajada_ns = ahora_ns;
        bool envia = b->fase == TALLER_UNHILO_ENVIAR || b->fase == TALLER_UNHILO_ESTADO ||
                     (b->fase == TALLER_UNHILO_BUSQUEDA && b->busqueda_paso < 2);
        b->lectura = envia;
        if (!envia) {
            return false;
        }
        bool linea = taller_unhilo_linea(b, ahora_ns);
        if (b->fase == TALLER_UNHILO_ENVIAR && ++b->bits >= b->total) {
            b->fase = TALLER_UNHILO_ESPERA;
        } else if (b->fase == TALLER_UNHILO_BUSQUEDA) {
            b->busqueda_paso++;
        }
        if (linea) {
            return false;
        }
        pulso->espera_ns = 0;
        pulso->duracion_ns = TALLER_UNHILO_CERO_NS;
        return true;
    }
    int64_t bajo = ahora_ns - b->bajada_ns;
    if (bajo >= TALLER_UNHILO_RESET_NS) {
        bool alguno = false;
        for (unsigned i = 0; i < TALLER_UNHILO_SENSORES; i++) {
            TallerUnHiloSensor *s = &b->s[i];
            if (s->usado) {
                taller_unhilo_terminar(s, ahora_ns);
                s->elegido = s->participa = false;
                alguno = true;
            }
        }
        b->fase = TALLER_UNHILO_ROM;
        b->bits = 0;
        b->byte = 0;
        b->lectura = false;
        if (!alguno) {
            return false;
        }
        pulso->espera_ns = TALLER_UNHILO_PRESENCIA_ESPERA_NS;
        pulso->duracion_ns = TALLER_UNHILO_PRESENCIA_NS;
        return true;
    }
    if (b->lectura) {
        b->lectura = false; /* el maestro suelta tras pedir un bit: no escribe nada */
        return false;
    }
    taller_unhilo_bit_recibido(b, bajo < TALLER_UNHILO_UNO_NS, ahora_ns);
    return false;
}

/*
 * Mira lo conducido (enable ? out : 1) en los pines de los buses. Devuelve cuántos pulsos ha
 * dejado en `pulsos` (uno por bus como mucho).
 */
static inline unsigned taller_unhilo_observar(TallerUnHilo *u, uint64_t out, uint64_t enable,
                                              int64_t ahora_ns,
                                              TallerUnHiloPulso pulsos[TALLER_UNHILO_BUSES])
{
    unsigned n = 0;
    for (unsigned i = 0; i < TALLER_UNHILO_BUSES; i++) {
        TallerUnHiloBus *b = &u->buses[i];
        if (!b->activo) {
            continue;
        }
        uint64_t bit = 1ull << b->pin;
        bool nivel = !(enable & bit) || (out & bit);
        if (taller_unhilo_cambio(b, nivel, ahora_ns, &pulsos[n])) {
            n++;
        }
    }
    return n;
}

/* Hasta cuándo hay que mantener el cuanto corto: 1 ms después del último cambio en algún bus. */
static inline int64_t taller_unhilo_cola_ns(const TallerUnHilo *u)
{
    int64_t fin = INT64_MIN;
    for (unsigned i = 0; i < TALLER_UNHILO_BUSES; i++) {
        const TallerUnHiloBus *b = &u->buses[i];
        if (b->activo && b->ultimo_cambio_ns && b->ultimo_cambio_ns + TALLER_UNHILO_COLA_NS > fin) {
            fin = b->ultimo_cambio_ns + TALLER_UNHILO_COLA_NS;
        }
    }
    return fin;
}

/* Carga de ENTRADA_TREN (pin, n = 2, bajo tras la espera y alto tras la duración). */
static inline size_t taller_unhilo_tren(const TallerUnHiloPulso *p, uint8_t carga[13])
{
    carga[0] = p->pin;
    carga[1] = 2;
    carga[2] = 0;
    const uint32_t esperas[2] = {p->espera_ns, p->duracion_ns};
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *q = carga + 3 + 5 * i;
        q[0] = (uint8_t)esperas[i];
        q[1] = (uint8_t)(esperas[i] >> 8);
        q[2] = (uint8_t)(esperas[i] >> 16);
        q[3] = (uint8_t)(esperas[i] >> 24);
        q[4] = i == 0 ? 0 : 1;
    }
    return 13;
}

#endif /* HW_MISC_TALLER_UNHILO_H */
