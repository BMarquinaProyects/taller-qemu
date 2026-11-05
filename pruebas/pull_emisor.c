/* Emisor PULL y saludo reales; solo transporte, reloj y temporizador son dobles. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int Chardev;
/* PROTOCOLO_REAL */
#define BUSES_SPI 4
#define QEMU_CLOCK_VIRTUAL 0
static int puente_fe;
static bool puente_activo;
static uint64_t pull_arriba, pull_abajo;
static TallerPuenteReceptor receptor_entrante;
static void *receptor_opaque;
static int error_fatal;
static void *latido;
static int64_t siguiente_latido;
static uint8_t salida[1024];
static size_t usados;
static unsigned vaciados;
static void stw_le_p(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void stl_le_p(uint8_t *p, uint32_t n)
{ stw_le_p(p, n); stw_le_p(p + 2, n >> 16); }
static void stq_le_p(uint8_t *p, uint64_t n)
{ stl_le_p(p, n); stl_le_p(p + 4, n >> 32); }
static int64_t qemu_clock_get_ns(int reloj) { return INT64_C(0x123456789); }
static int qemu_chr_fe_write_all(int *fe, const uint8_t *p, size_t n)
{
    assert(usados + n <= sizeof(salida));
    memcpy(salida + usados, p, n);
    usados += n;
    return n;
}
static void qemu_chr_fe_init(int *fe, Chardev *chr, int *err) {}
#define qemu_chr_fe_set_handlers(...) ((void)0)
static void latir(void *op) {}
static void *timer_new_ns(int reloj, void (*cb)(void *), void *op) { return &usados; }
static void timer_mod_ns(void *t, int64_t plazo) {}
void taller_puente_spi_fin(uint8_t bus) { assert(bus < 4); vaciados++; }

/* EMISOR_REAL */

static void comprobar(size_t offset, uint8_t tipo, unsigned largo)
{
    uint8_t tiempo[8];
    stq_le_p(tiempo, INT64_C(0x123456789));
    assert(salida[offset] == tipo && salida[offset + 1] == TALLER_PUENTE_VERSION);
    assert(salida[offset + 2] == largo && salida[offset + 3] == 0);
    assert(!memcmp(salida + offset + 4, tiempo, 8));
}

int main(void)
{
    taller_puente_iniciar(NULL, NULL, NULL);
    comprobar(0, 1, 4);
    assert(salida[12] == TALLER_PUENTE_VERSION && !salida[13] && !salida[14] && !salida[15]);
    comprobar(16, 9, 16);
    assert(usados == 44);
    for (unsigned i = 28; i < 44; i++) { assert(salida[i] == 0); }
    taller_puente_pull(0, 0);
    assert(usados == 44);
    taller_puente_pull(1ull << 48, 1ull << 33);
    comprobar(44, 9, 16);
    uint8_t esperado[16];
    stq_le_p(esperado, 1ull << 48);
    stq_le_p(esperado + 8, 1ull << 33);
    assert(!memcmp(salida + 56, esperado, 16) && usados == 72);
    taller_puente_pull(1ull << 48, 1ull << 33);
    assert(usados == 72);
    taller_puente_pull(1ull << 48, 0); /* Cambiar solo abajo también avisa. */
    taller_puente_pull(0, 0); /* Reset, una sola trama. */
    taller_puente_pull(0, 0);
    assert(usados == 128 && vaciados == 16);
    puente_activo = false;
    usados = 0;
    taller_puente_pull(1ull << 15, 1ull << 16);
    assert(!usados);
    taller_puente_iniciar(NULL, NULL, NULL);
    comprobar(0, 1, 4);
    comprobar(16, 9, 16);
    stq_le_p(esperado, 1ull << 15);
    stq_le_p(esperado + 8, 1ull << 16);
    assert(usados == 44 && !memcmp(salida + 28, esperado, 16));
    puts("OK emisor PULL v8: HOLA, estado previo, cambios, LE y tiempo virtual");
    return 0;
}
