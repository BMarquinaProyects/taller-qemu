/* Se compila junto al emisor REAL del puente, sustituyendo solo chardev/SPI. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TALLER_PUENTE_VERSION 6
#define TALLER_TRAMA_RMT 7
#define BUSES_SPI 4
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static void liberar(uint8_t **p) { free(*p); }
#define g_autofree __attribute__((cleanup(liberar)))
#define g_malloc malloc
static bool puente_activo;
static int puente_fe;
static unsigned spi_vaciados;
static uint8_t salida[200000];
static size_t usados;
static void stw_le_p(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void stl_le_p(uint8_t *p, uint32_t n)
{
    stw_le_p(p, n); stw_le_p(p + 2, n >> 16);
}
static void stq_le_p(uint8_t *p, uint64_t n)
{
    stl_le_p(p, n); stl_le_p(p + 4, n >> 32);
}
static int qemu_chr_fe_write_all(int *fe, const uint8_t *p, unsigned n)
{
    (void)fe;
    assert(usados + n <= sizeof(salida));
    memcpy(salida + usados, p, n);
    usados += n;
    return n;
}
static void taller_puente_spi_fin(unsigned bus)
{
    assert(bus < BUSES_SPI);
    spi_vaciados++;
}

/* EMISOR_REAL */

int main(void)
{
    uint32_t datos[20000];
    for (unsigned i = 0; i < 20000; i++) {
        datos[i] = 0x12340000u + i;
    }
    taller_puente_rmt(7, 255, 12500, datos, 20000, 0x123456789);
    assert(usados == 0);
    puente_activo = true;
    taller_puente_rmt(7, 255, 12500, datos, 20000, 0x123456789);
    assert(spi_vaciados == 4);
    size_t off = 0;
    unsigned indice = 0, tramas = 0;
    while (off < usados) {
        const uint8_t *p = salida + off;
        unsigned largo = p[2] | (p[3] << 8);
        uint8_t esperado[8];
        assert(p[0] == 7 && p[1] == 6 && largo <= 65535 && (largo - 6) % 4 == 0);
        stq_le_p(esperado, 0x123456789);
        assert(memcmp(p + 4, esperado, 8) == 0);
        assert(p[12] == 7 && p[13] == 255);
        stl_le_p(esperado, 12500);
        assert(memcmp(p + 14, esperado, 4) == 0);
        for (unsigned j = 6; j < largo; j += 4) {
            stl_le_p(esperado, datos[indice++]);
            assert(memcmp(p + 12 + j, esperado, 4) == 0);
        }
        off += 12 + largo;
        tramas++;
    }
    assert(tramas == 2 && indice == 20000);
    puts("OK emisor RMT: v7, LE, tiempo inicial y troceado de 20000 símbolos");
}
