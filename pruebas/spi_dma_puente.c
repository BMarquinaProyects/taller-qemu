/* Emisor SPI real del puente: diez tramas, sin pérdida ni repetición. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUSES_SPI 4
#define CARGA_MAX_SPI 4096
#define TALLER_TRAMA_SPI 3
static bool puente_activo = true;
static uint8_t spi_buf[BUSES_SPI][1 + CARGA_MAX_SPI];
static uint16_t spi_len[BUSES_SPI];
static uint8_t recibido[40960];
static size_t total, tramas;

static void enviar(uint8_t tipo, const uint8_t *carga, uint16_t largo)
{
    assert(tipo == 3 && carga[0] == 3 && largo == 4097);
    assert(total + largo - 1 <= sizeof(recibido));
    memcpy(recibido + total, carga + 1, largo - 1);
    total += largo - 1;
    tramas++;
}

/* EMISOR_REAL */

int main(void)
{
    for (unsigned i = 0; i < 40960; i++) { taller_puente_spi_byte(3, i & 255); }
    taller_puente_spi_fin(3);
    taller_puente_spi_fin(3); /* No enviar una trama vacía extra. */
    assert(total == 40960 && tramas == 10);
    for (unsigned i = 0; i < 40960; i++) { assert(recibido[i] == (i & 255)); }
    puts("OK puente SPI real: 40960 bytes exactos en diez tramas v7");
    return 0;
}
