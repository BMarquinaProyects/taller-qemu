/* I2C C3 real: el adaptador conserva la selección del core QEMU entre START.
 * Así detecta un RSTART que omita FINISH y reutilice el esclavo anterior.
 * Los valores esperados se deducen del HAL C3 de IDF 5.5, no del modelo.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef uint64_t hwaddr;
typedef int qemu_irq;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
typedef struct I2CBus {
    uint8_t espejo[256];
    uint8_t direccion, puntero;
    bool activo, seleccionado, lectura, espera_puntero;
    unsigned inicios, finales, nacks, enviados;
    int rechazar_dato;
} I2CBus;
static int nivel_irq;
static void qemu_set_irq(qemu_irq irq, int nivel) { nivel_irq = !!nivel; }
static int i2c_start_transfer(I2CBus *b, uint8_t direccion, bool lectura)
{
    b->inicios++;
    if (!b->seleccionado && (!b->activo || direccion != b->direccion)) {
        return 1;
    }
    b->seleccionado = true;
    b->lectura = lectura;
    b->espera_puntero = !lectura;
    return 0;
}
static void i2c_end_transfer(I2CBus *b)
{
    if (b->seleccionado) {
        b->finales++;
    }
    b->seleccionado = false;
}
static int i2c_send(I2CBus *b, uint8_t dato)
{
    if (!b->seleccionado || b->lectura || b->rechazar_dato == dato) {
        return 1;
    }
    b->enviados++;
    if (b->espera_puntero) {
        b->puntero = dato;
        b->espera_puntero = false;
    } else {
        b->espejo[b->puntero++] = dato;
    }
    return 0;
}
static uint8_t i2c_recv(I2CBus *b)
{
    return b->seleccionado && b->lectura ? b->espejo[b->puntero++] : 0xff;
}
static void i2c_nack(I2CBus *b) { b->nacks++; }
/* CABECERA_REAL */
/* MODELO_REAL */
static I2CBus bus = { .direccion = 0x3c, .activo = true, .rechazar_dato = -1 };
static ESP32C3I2CState i2c;
static void escribir(hwaddr a, uint32_t v) { esp32c3_i2c_write(&i2c, a, v, 4); }
static uint32_t leer(hwaddr a) { return (uint32_t)esp32c3_i2c_read(&i2c, a, 4); }
static void limpiar(void)
{
    esp32c3_i2c_reiniciar(&i2c, 0);
    escribir(0x18, (1u << 14) | (4u << 5) | 3);
    escribir(0x24, 0x3ffff);
}
static void comando(unsigned indice, unsigned op, unsigned cantidad, unsigned ack)
{
    escribir(0x58 + 4 * indice, (op << 11) | cantidad | ack);
}
static void arrancar(void) { escribir(4, (1u << 4) | (1u << 5)); }
static void dato(uint32_t v) { escribir(0x1c, v); }
static void sondear(uint8_t direccion)
{
    dato(direccion << 1);
    comando(0, 6, 0, 0);
    comando(1, 1, 1, 1u << 8);
    comando(2, 2, 0, 0);
    arrancar();
}
static void prueba_sondeo_y_nack(void)
{
    limpiar();
    sondear(0x3c);
    assert((leer(0x20) & 0x488) == 0x80);
    assert((leer(8) & 0x10) == 0 && !bus.seleccionado);
    assert(leer(8) & 1); /* RESP_REC = 1: el esclavo dijo ACK (como el silicio, no como el .h). */
    assert(leer(0x58) >> 31 && leer(0x5c) >> 31 && leer(0x60) >> 31);
    assert(!nivel_irq && leer(0x2c) == 0);
    escribir(0x28, 0x480);
    assert(nivel_irq && leer(0x2c) == 0x80);
    escribir(0x24, 0x80);
    assert(!nivel_irq);
    sondear(0x48);
    assert((leer(0x20) & 0x488) == 0x400);
    assert(nivel_irq && !(leer(8) & 1)); /* NACK: RESP_REC = 0. */
    assert(!(leer(0x60) >> 31)); /* STOP posterior a NACK no se ejecuta. */
    escribir(0x24, 0x400);
    assert(!nivel_irq && leer(0x24) == 0);
    /* El NACK de un byte de datos también debe detener el programa. */
    limpiar();
    bus.rechazar_dato = 0xee;
    dato(0x78); dato(0); dato(0xee);
    comando(0, 6, 0, 0);
    comando(1, 1, 3, 1u << 8);
    comando(2, 2, 0, 0);
    arrancar();
    assert((leer(0x20) & 0x480) == 0x400);
    bus.rechazar_dato = -1;
}
static void prueba_escritura_y_rstart(void)
{
    limpiar();
    dato(0x78); dato(0x20); dato(0xaf); dato(0x79);
    comando(0, 6, 0, 0);
    comando(1, 1, 3, 1u << 8);
    comando(2, 6, 0, 0);
    comando(3, 1, 1, 1u << 8);
    comando(4, 3, 1, 1u << 10);
    comando(5, 2, 0, 0);
    bus.espejo[0x21] = 0x61;
    unsigned nacks = bus.nacks;
    arrancar();
    assert(bus.espejo[0x20] == 0xaf && leer(0x1c) == 0x61);
    assert(bus.nacks == nacks + 1 && (leer(0x20) & 0x488) == 0x80);
    /* END retiene la selección; otro RSTART debe resolver otra dirección. */
    limpiar();
    dato(0x78); dato(0x20);
    comando(0, 6, 0, 0); comando(1, 1, 2, 1u << 8); comando(2, 4, 0, 0);
    arrancar();
    assert((leer(0x20) & 0x488) == 8 && (leer(8) & 0x10));
    escribir(0x24, 8);
    dato(0x91); /* La dirección 0x48 no existe. */
    comando(0, 6, 0, 0); comando(1, 1, 1, 1u << 8); comando(2, 2, 0, 0);
    arrancar();
    assert((leer(0x20) & 0x488) == 0x400 && !bus.seleccionado);
}
static void prueba_continuaciones(void)
{
    limpiar();
    dato(0x78); dato(0x40);
    for (unsigned n = 0; n < 30; n++) { dato(0x80 + n); }
    assert(((leer(8) >> 18) & 63) == 32);
    comando(0, 6, 0, 0); comando(1, 1, 32, 1u << 8); comando(2, 4, 0, 0);
    unsigned inicios = bus.inicios;
    arrancar();
    assert((leer(0x20) & 0x488) == 8 && bus.seleccionado);
    escribir(0x24, 8);
    for (unsigned n = 30; n < 70; n++) {
        dato(0x80 + n);
        if (n == 61 || n == 69) {
            comando(0, 1, n == 61 ? 32 : 8, 1u << 8);
            comando(1, n == 61 ? 4 : 2, 0, 0);
            arrancar();
            escribir(0x24, 8);
        }
    }
    assert(bus.inicios == inicios + 1 && !bus.seleccionado);
    for (unsigned n = 0; n < 70; n++) { assert(bus.espejo[0x40 + n] == 0x80 + n); }
    assert((leer(0x20) & 0x488) == 0x80);
    /* Lectura en tres lotes sin introducir dirección ni STOP entre END. */
    limpiar();
    dato(0x78); dato(0x40); dato(0x79);
    comando(0, 6, 0, 0); comando(1, 1, 2, 1u << 8);
    comando(2, 6, 0, 0); comando(3, 1, 1, 1u << 8);
    comando(4, 3, 32, 0); comando(5, 4, 0, 0);
    arrancar();
    assert(((leer(8) >> 8) & 63) == 32);
    for (unsigned n = 0; n < 32; n++) { assert(leer(0x1c) == 0x80 + n); }
    escribir(0x24, 8);
    comando(0, 3, 32, 0); comando(1, 4, 0, 0); arrancar();
    for (unsigned n = 32; n < 64; n++) { assert(leer(0x1c) == 0x80 + n); }
    escribir(0x24, 8);
    comando(0, 3, 5, 0); comando(1, 3, 1, 1u << 10); comando(2, 2, 0, 0);
    arrancar();
    for (unsigned n = 64; n < 70; n++) { assert(leer(0x1c) == 0x80 + n); }
    assert(!bus.seleccionado && (leer(0x20) & 0x488) == 0x80);
}
static void prueba_fifo_irq_y_reset(void)
{
    limpiar();
    escribir(0x28, 3);
    assert((leer(0x2c) & 3) == 2 && nivel_irq);
    for (unsigned n = 0; n < 5; n++) { dato(n); }
    assert(!(leer(0x2c) & 2) && !nivel_irq);
    escribir(0x18, (1u << 14) | (4u << 5) | 3 | (1u << 13));
    assert(((leer(8) >> 18) & 63) == 0 && (leer(0x2c) & 2));
    escribir(0x28, 1);
    dato(0x79);
    comando(0, 6, 0, 0); comando(1, 1, 1, 1u << 8);
    comando(2, 3, 3, 0); comando(3, 4, 0, 0); arrancar();
    assert((leer(0x2c) & 1) && nivel_irq);
    (void)leer(0x1c);
    assert(!(leer(0x2c) & 1) && !nivel_irq);
    escribir(0x18, (1u << 14) | (4u << 5) | 3 | (1u << 12));
    assert(((leer(8) >> 8) & 63) == 0);
    escribir(0, 123); escribir(4, (1u << 4) | (1u << 11));
    assert(leer(4) == (1u << 4));
    escribir(4, (1u << 4) | (1u << 10));
    assert(leer(4) == (1u << 4) && !(leer(8) & 0x10));
    assert(leer(0) == 123 && !bus.seleccionado);
    escribir(0x80, 19); assert(leer(0x80) == 18); /* Pulso de recuperación. */
    escribir(0x20, 0xffffffff); escribir(0x2c, 0xffffffff);
    escribir(0x300, 0xffffffff); escribir(1, 0xffffffff);
    assert(leer(0x300) == 0 && leer(1) == 0);
    esp32c3_i2c_reiniciar(&i2c, 0);
    assert(leer(0) == 0 && leer(0x28) == 0 && !nivel_irq);
    assert(!(leer(8) & 0xffffff) && !bus.seleccionado);
    assert(bus.espejo[0x40] == 0x80 && bus.espejo[0x85] == 0xc5);
}
static void prueba_comando_mayor_que_fifo(void)
{
    limpiar();
    dato(0x78); dato(0xa0);
    for (unsigned n = 0; n < 30; n++) { dato(n + 1); }
    comando(0, 6, 0, 0); comando(1, 1, 42, 1u << 8); comando(2, 2, 0, 0);
    arrancar();
    assert(!(leer(0x5c) >> 31) && !(leer(0x20) & 0x480));
    for (unsigned n = 30; n < 40; n++) { dato(n + 1); }
    assert(leer(0x5c) >> 31 && (leer(0x20) & 0x480) == 0x80);
    for (unsigned n = 0; n < 40; n++) { assert(bus.espejo[0xa0 + n] == n + 1); }
    /* Un único READ puede continuar al vaciar DATA, sin perder bytes. */
    limpiar();
    dato(0x78); dato(0xa0); dato(0x79);
    comando(0, 6, 0, 0); comando(1, 1, 2, 1u << 8);
    comando(2, 6, 0, 0); comando(3, 1, 1, 1u << 8);
    comando(4, 3, 39, 0); comando(5, 3, 1, 1u << 10); comando(6, 2, 0, 0);
    arrancar();
    assert(((leer(8) >> 8) & 63) == 32 && !(leer(0x68) >> 31));
    for (unsigned n = 0; n < 40; n++) { assert(leer(0x1c) == n + 1); }
    assert((leer(0x20) & 0x488) == 0x80 && !bus.seleccionado);
    /* FIFO llena/vacía no debe desbordar memoria ni inventar datos. */
    limpiar();
    for (unsigned n = 0; n < 33; n++) { dato(n); }
    assert(((leer(8) >> 18) & 63) == 32 && (leer(0x20) & (1u << 11)));
    assert(leer(0x1c) == 0 && (leer(0x20) & (1u << 12)));
}
int main(void)
{
    i2c.bus = &bus;
    prueba_sondeo_y_nack();
    prueba_escritura_y_rstart();
    prueba_continuaciones();
    prueba_fifo_irq_y_reset();
    prueba_comando_mayor_que_fifo();
    puts("OK I2C C3 real: sondeo, NACK, RSTART, FIFO, END, IRQ y reset");
    return 0;
}
