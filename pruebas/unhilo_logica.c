/* Lógica pura de 1-Wire (trama 22 UNHILO, v9): un maestro simulado con los tiempos de la
 * librería OneWire contra uno o dos DS18B20. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_unhilo.h"

#define PIN 4
#define US INT64_C(1000)

static TallerUnHilo u;
static int64_t t;
static bool conduce_bajo;
static int64_t ventana_desde = -1, ventana_hasta = -1;

static void conducir(bool nivel)
{
    TallerUnHiloPulso p[TALLER_UNHILO_BUSES];
    conduce_bajo = !nivel;
    uint64_t enable = nivel ? 0 : (1ull << PIN); /* suelta = entrada; bajo = salida a 0 */
    unsigned n = taller_unhilo_observar(&u, 0, enable, t, p);
    for (unsigned i = 0; i < n; i++) {
        assert(p[i].pin == PIN);
        ventana_desde = t + p[i].espera_ns;
        ventana_hasta = ventana_desde + p[i].duracion_ns;
    }
}

static bool linea(int64_t x)
{
    return !(conduce_bajo || (x >= ventana_desde && x < ventana_hasta));
}

static bool reset(void)
{
    conducir(false);
    t += 480 * US;
    conducir(true);
    bool presencia = !linea(t + 70 * US);
    t += 480 * US;
    return presencia;
}

static void escribir_bit(bool b)
{
    conducir(false);
    t += b ? 6 * US : 60 * US;
    conducir(true);
    t += b ? 64 * US : 10 * US;
}

static bool leer_bit(void)
{
    conducir(false);
    t += 3 * US;
    conducir(true);
    bool b = linea(t + 10 * US);
    t += 57 * US;
    return b;
}

static void escribir(uint8_t v)
{
    for (unsigned i = 0; i < 8; i++) {
        escribir_bit((v >> i) & 1);
    }
}

static uint8_t leer(void)
{
    uint8_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
        v |= (uint8_t)(leer_bit() << i);
    }
    return v;
}

static void alta(const uint8_t rom[8], int16_t medida)
{
    uint8_t c[TALLER_UNHILO_CARGA] = {PIN};
    for (unsigned i = 0; i < 8; i++) {
        c[1 + i] = rom[i];
    }
    c[9] = (uint8_t)medida;
    c[10] = (uint8_t)((uint16_t)medida >> 8);
    c[11] = 1;
    assert(taller_unhilo_recibir(&u, c, sizeof(c), UINT64_C(0xffffffffff)));
}

static void rom_con_crc(uint8_t rom[8], uint8_t serie)
{
    const uint8_t base[7] = {0x28, serie, 0x4B, 0x46, 0x7F, 0x0C, 0x10};
    for (unsigned i = 0; i < 7; i++) {
        rom[i] = base[i];
    }
    rom[7] = taller_unhilo_crc8(rom, 7);
}

static void leer_scratchpad(uint8_t sp[9])
{
    assert(reset());
    escribir(0xCC);
    escribir(0xBE);
    for (unsigned i = 0; i < 9; i++) {
        sp[i] = leer();
    }
}

static void crc_de_la_hoja(void)
{
    /* AN27 de Maxim: familia 02, serie 00000001B81C → CRC A2. */
    const uint8_t rom[7] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};
    assert(taller_unhilo_crc8(rom, 7) == 0xA2);
    /* Scratchpad de fábrica (medido por cpetrich): 50 05 4B 46 7F FF 0C 10 → 1C. */
    const uint8_t sp[8] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10};
    assert(taller_unhilo_crc8(sp, 8) == 0x1C);
}

static void sin_sensores_no_hay_presencia(void)
{
    taller_unhilo_iniciar(&u);
    t = 1000 * US;
    assert(!reset());
}

static void conversion_y_lectura(void)
{
    uint8_t rom[8], sp[9];
    taller_unhilo_iniciar(&u);
    t = 1000 * US;
    rom_con_crc(rom, 0x11);
    alta(rom, 344); /* 21,5 °C */
    leer_scratchpad(sp);
    assert(sp[0] == 0x50 && sp[1] == 0x05 && "al encender, 85 °C");
    assert(sp[8] == taller_unhilo_crc8(sp, 8));
    assert(reset());
    escribir(0xCC);
    escribir(0x44);
    assert(!leer_bit() && "convirtiendo");
    int64_t inicio = t;
    while (!leer_bit()) {
        t += 1000 * US;
    }
    assert(t - inicio >= 590000 * US && t - inicio <= 610000 * US && "~600 ms a 12 bits");
    leer_scratchpad(sp);
    assert(sp[0] == 0x58 && sp[1] == 0x01);
    assert(sp[4] == 0x7F && sp[8] == taller_unhilo_crc8(sp, 8));
    /* Read ROM con un solo sensor. */
    assert(reset());
    escribir(0x33);
    for (unsigned i = 0; i < 8; i++) {
        assert(leer() == rom[i]);
    }
}

static void resolucion_de_9_bits(void)
{
    uint8_t rom[8], sp[9];
    taller_unhilo_iniciar(&u);
    t = 1000 * US;
    rom_con_crc(rom, 0x22);
    alta(rom, 345); /* 21,5625 °C */
    assert(reset());
    escribir(0xCC);
    escribir(0x4E);
    escribir(30);
    escribir(10);
    escribir(0x1F);
    assert(reset());
    escribir(0xCC);
    escribir(0x44);
    int64_t inicio = t;
    while (!leer_bit()) {
        t += 1000 * US;
    }
    assert(t - inicio <= 76000 * US && "75 ms a 9 bits");
    leer_scratchpad(sp);
    assert(sp[0] == 0x58 && sp[1] == 0x01 && "los 3 bits bajos, a 0");
    assert(sp[2] == 30 && sp[3] == 10 && sp[4] == 0x1F);
}

static void busqueda_y_match_con_dos(void)
{
    uint8_t a[8], b[8], rom[8] = {0};
    taller_unhilo_iniciar(&u);
    t = 1000 * US;
    rom_con_crc(a, 0x31);
    rom_con_crc(b, 0x32);
    alta(a, 160);  /* 10 °C */
    alta(b, 480);  /* 30 °C */
    /* Búsqueda de la librería OneWire (AN187). */
    int ultima = -1, encontrados = 0;
    bool fin = false;
    while (!fin) {
        assert(reset());
        escribir(0xF0);
        int ultimo_cero = -1;
        for (int i = 0; i < 64; i++) {
            bool bit = leer_bit(), comp = leer_bit();
            assert(!(bit && comp) && "alguien contesta siempre");
            bool dir;
            if (bit != comp) {
                dir = bit;
            } else {
                dir = i < ultima ? ((rom[i / 8] >> (i % 8)) & 1) : i == ultima;
                if (!dir) {
                    ultimo_cero = i;
                }
            }
            rom[i / 8] = (uint8_t)((rom[i / 8] & ~(1u << (i % 8))) | ((unsigned)dir << (i % 8)));
            escribir_bit(dir);
        }
        assert(!memcmp(rom, a, 8) || !memcmp(rom, b, 8));
        encontrados++;
        ultima = ultimo_cero;
        fin = ultima < 0;
    }
    assert(encontrados == 2);
    /* Match ROM: solo el elegido convierte y contesta. */
    assert(reset());
    escribir(0xCC);
    escribir(0x44);
    t += 700000 * US;
    assert(reset());
    escribir(0x55);
    for (unsigned i = 0; i < 8; i++) {
        escribir(b[i]);
    }
    escribir(0xBE);
    assert(leer() == 0xE0 && leer() == 0x01 && "30 °C del segundo");
    /* Alarm Search: con TH = 75 y TL = 70 de fábrica, los dos están por debajo de TL. */
    assert(reset());
    escribir(0xEC);
    bool bit = leer_bit(), comp = leer_bit();
    assert(!(bit && comp));
}

static void la_trama_de_baja_quita_el_sensor(void)
{
    uint8_t rom[8], c[TALLER_UNHILO_CARGA] = {PIN};
    taller_unhilo_iniciar(&u);
    t = 1000 * US;
    rom_con_crc(rom, 0x41);
    alta(rom, 0);
    for (unsigned i = 0; i < 8; i++) {
        c[1 + i] = rom[i];
    }
    assert(taller_unhilo_recibir(&u, c, sizeof(c), UINT64_C(0xffffffffff)));
    assert(!u.buses[0].activo);
    assert(!reset());
    assert(!taller_unhilo_recibir(&u, c, 5, UINT64_C(0xffffffffff)) && "largo malo");
    c[0] = 45;
    c[11] = 1;
    assert(!taller_unhilo_recibir(&u, c, sizeof(c), UINT64_C(0xffffffffff)) && "pin fuera del chip");
}

int main(void)
{
    crc_de_la_hoja();
    sin_sensores_no_hay_presencia();
    conversion_y_lectura();
    resolucion_de_9_bits();
    busqueda_y_match_con_dos();
    la_trama_de_baja_quita_el_sensor();
    puts("unhilo: ok");
    return 0;
}
