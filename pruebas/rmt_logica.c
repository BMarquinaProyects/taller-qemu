/* Comportamiento de la lógica real, sin dependencias de QEMU. */
#include <assert.h>
#include <stdio.h>
#include "hw/misc/taller_rmt.h"

static uint32_t simbolo(unsigned n)
{
    return (n + 1) | (1u << 15) | (2u << 16);
}

static void ping_pong(unsigned canal, unsigned bloques, unsigned cantidad)
{
    TallerRmt s;
    unsigned capacidad = bloques * 64, mitad = capacidad / 2;
    unsigned base = canal * 64, siguiente = capacidad, avisos = 0;
    uint64_t ticks = 0;
    taller_rmt_iniciar(&s);
    taller_rmt_escribir(&s, 0x20 + canal * 8, 8 | (bloques << 24));
    taller_rmt_escribir(&s, 0xf0, 3);
    taller_rmt_escribir(&s, 0xd0 + canal * 4, mitad);
    for (unsigned i = 0; i < capacidad; i++) {
        s.memoria[base + i] = simbolo(i);
    }
    assert(taller_rmt_escribir(&s, 0x24 + canal * 8,
                              (1u << 17) | 8 | 1) == (1u << canal));
    assert(s.tx[canal].tick_ps == 100000);
    for (unsigned i = 0; i <= cantidad; i++) {
        TallerRmtPaso p = taller_rmt_cargar(&s, canal);
        assert(p.valido);
        assert(p.simbolo == (i == cantidad ? 0 : simbolo(i)));
        ticks += p.ticks;
        unsigned evento = taller_rmt_avanzar(&s, canal, p);
        if (i == cantidad) {
            assert(evento & TALLER_RMT_FIN);
            assert(!s.tx[canal].activo);
            break;
        }
        assert(!(evento & TALLER_RMT_FIN));
        if ((i + 1) % mitad == 0) {
            assert(evento & TALLER_RMT_UMBRAL);
            assert(s.registros[0xa0 / 4] & (1u << (24 + canal)));
            /* ISR: limpiar RAW, rellenar la mitad que acaba de salir. */
            taller_rmt_escribir(&s, 0xac, 1u << (24 + canal));
            unsigned inicio = (avisos++ % 2) * mitad;
            for (unsigned j = 0; j < mitad; j++, siguiente++) {
                s.memoria[base + inicio + j] = siguiente < cantidad ? simbolo(siguiente) : 0;
            }
        } else {
            assert(!(evento & TALLER_RMT_UMBRAL));
        }
    }
    assert(avisos == cantidad / mitad);
    assert(ticks == (uint64_t)cantidad * (cantidad + 1) / 2 + 2 * cantidad);
    assert(s.tx[canal].tiempo_ps == ticks * 100000);
    assert(s.registros[0xa0 / 4] & (1u << (3 * canal)));
}

static void fin_relojes_irq_y_registros(void)
{
    TallerRmt s;
    taller_rmt_iniciar(&s);
    assert(taller_rmt_tick_ps(1, true) == 12500);
    assert(taller_rmt_tick_ps(8, true) == 100000);
    assert(taller_rmt_tick_ps(0, true) == 3200000);
    assert(taller_rmt_tick_ps(1, false) == 1000000);
    assert(taller_rmt_tick_ps(0, false) == 256000000);
    taller_rmt_escribir(&s, 0x20, 1 | (1u << 24));
    taller_rmt_escribir(&s, 0xf0, 1);
    taller_rmt_escribir(&s, 0x800, 3 | (1u << 15)); /* Solo primera mitad. */
    assert(taller_rmt_leer(&s, 0x800) == (3 | (1u << 15)));
    taller_rmt_escribir(&s, 0x24, (1u << 17) | 1);
    TallerRmtPaso p = taller_rmt_cargar(&s, 0);
    assert(p.ticks == 3 && p.fin);
    assert(s.tx[0].activo && taller_rmt_leer(&s, 0xa0) == 0);
    assert(s.tx[0].tiempo_ps == 37500); /* Redondear solo plazo total a ns. */
    assert(taller_rmt_avanzar(&s, 0, p) == TALLER_RMT_FIN);
    assert(taller_rmt_leer(&s, 0xa0) == 1);
    assert(taller_rmt_leer(&s, 0xa4) == 0); /* RAW independiente de ENA. */
    taller_rmt_escribir(&s, 0xa8, 1);
    assert(taller_rmt_leer(&s, 0xa4) == 1);
    taller_rmt_escribir(&s, 0xac, 1);
    assert(taller_rmt_leer(&s, 0xa4) == 0);
    taller_rmt_escribir(&s, 0xa0, ~0u); /* Solo lectura. */
    assert(taller_rmt_leer(&s, 0xa0) == 0);
    taller_rmt_escribir(&s, 0x24, (1u << 17) | (1u << 5) | 1);
    /* MEM_OWNER = 1 no bloquea la TX (así lo deja el driver de IDF 5.5; ver tx_con_mem_owner_a_uno). */
    assert(s.tx[0].activo);
    assert(!(taller_rmt_leer(&s, 0xa0) & 4));
    taller_rmt_iniciar(&s);
    assert(taller_rmt_leer(&s, 0xa8) == 0 && !s.tx[0].activo);
}

static void acceso_ram_y_reset(void)
{
    TallerRmt s;
    taller_rmt_iniciar(&s);
    assert(taller_rmt_leer(&s, 0x24) == 0xf20);
    assert(taller_rmt_leer(&s, 0xfc) == 0x16022600);
    /* FIFO por defecto; el HAL selecciona acceso directo con FIFO_MASK. */
    taller_rmt_escribir(&s, 0x800, 123);
    assert(taller_rmt_leer(&s, 0x800) == 0);
    taller_rmt_escribir(&s, 0, 42);
    assert(taller_rmt_leer(&s, 0) == 42);
    taller_rmt_escribir(&s, 0xf0, 1);
    taller_rmt_escribir(&s, 0, 99);
    taller_rmt_escribir(&s, 0x804, 17);
    assert(taller_rmt_leer(&s, 0) == 0);
    assert(taller_rmt_leer(&s, 0x800) == 42);
    assert(taller_rmt_leer(&s, 0x804) == 17);
    taller_rmt_iniciar(&s);
    assert(s.memoria[0] == 0 && s.memoria[1] == 0);
    for (unsigned c = 0; c < 8; c++) {
        assert(!s.tx[c].activo);
        taller_rmt_escribir(&s, 0x24 + c * 8, 1 | 8 | (1u << 17));
    }
    /* Ocho canales simultáneos: terminar uno no altera el resto. */
    for (unsigned c = 0; c < 8; c++) {
        assert(s.tx[c].activo);
        assert(taller_rmt_avanzar(&s, c, taller_rmt_cargar(&s, c)) == TALLER_RMT_FIN);
        assert(taller_rmt_leer(&s, 0xa0) & (1u << (3 * c)));
    }
}

static void limites_bucle_y_umbral(void)
{
    TallerRmt s;
    taller_rmt_iniciar(&s);
    taller_rmt_escribir(&s, 0x20, 1 | (1u << 24));
    for (unsigned i = 0; i < 64; i++) {
        s.memoria[i] = simbolo(0);
    }
    taller_rmt_escribir(&s, 0xd0, 3);
    taller_rmt_escribir(&s, 0x24, 1);
    for (unsigned i = 0; i < 64; i++) {
        unsigned e = taller_rmt_avanzar(&s, 0, taller_rmt_cargar(&s, 0));
        assert(!!(e & TALLER_RMT_UMBRAL) == ((i + 1) % 3 == 0));
        assert(!!(e & TALLER_RMT_FIN) == (i == 63));
    }
    assert(taller_rmt_leer(&s, 0x60) & (1u << 29));
    /* Reset de puntero y umbral no divisor: vuelta tras fin de media palabra. */
    s.memoria[0] = 5;
    taller_rmt_escribir(&s, 0x24, 1 | 8 | 64);
    assert(s.tx[0].posicion == 0 && s.tx[0].desde_umbral == 0);
    assert(taller_rmt_avanzar(&s, 0, taller_rmt_cargar(&s, 0)) == TALLER_RMT_VUELTA);
    assert(s.tx[0].activo && s.tx[0].posicion == 0);
    taller_rmt_escribir(&s, 0x24, 0); /* Desactivar continuo no aborta. */
    assert(taller_rmt_avanzar(&s, 0, taller_rmt_cargar(&s, 0)) & TALLER_RMT_FIN);
    /* Fin inmediato, tx_lim=0, canal 7 y tamaño inválido: sin salir de RAM. */
    taller_rmt_escribir(&s, 0x5c, 1);
    assert(taller_rmt_avanzar(&s, 7, taller_rmt_cargar(&s, 7)) == TALLER_RMT_FIN);
    taller_rmt_escribir(&s, 0x58, 0);
    taller_rmt_escribir(&s, 0x5c, 1);
    assert(!s.tx[7].activo && (taller_rmt_leer(&s, 0xa0) & (1u << 23)));
}

/* Secuencia real del driver de IDF 5.5 (traza de NeoPixel): TX_START con MEM_OWNER = 1. */
static void tx_con_mem_owner_a_uno(void)
{
    TallerRmt s;
    taller_rmt_iniciar(&s);
    taller_rmt_escribir(&s, 0x20, 0x21100008);   /* divisor 8, mem_size 1 */
    taller_rmt_escribir(&s, 0xf0, 3);            /* RAM directa + wrap */
    taller_rmt_escribir(&s, 0x800, 4 | (1u << 15) | (8u << 16));
    taller_rmt_escribir(&s, 0x804, 0);
    assert(taller_rmt_escribir(&s, 0x24, 0x000a0f21) == 1);
    assert(s.tx[0].activo);
    assert(!(taller_rmt_leer(&s, 0xa0) & 4)); /* sin error */
    TallerRmtPaso p = taller_rmt_cargar(&s, 0);
    assert(p.ticks == 12 && !p.fin);
    taller_rmt_avanzar(&s, 0, p);
    p = taller_rmt_cargar(&s, 0);
    assert(p.fin);
    assert(taller_rmt_avanzar(&s, 0, p) & TALLER_RMT_FIN);
    assert(taller_rmt_leer(&s, 0xa0) & 1); /* tx_end */
}

int main(void)
{
    tx_con_mem_owner_a_uno();
    ping_pong(0, 1, 192);
    ping_pong(3, 1, 720);
    ping_pong(6, 2, 720);
    fin_relojes_irq_y_registros();
    limites_bucle_y_umbral();
    acceso_ram_y_reset();
    puts("OK RMT: ping-pong 192/720, multibloque, umbral, fin, relojes e IRQ");
}
