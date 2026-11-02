/* Modelo real RMT C3/S3: sustituye solo QOM, temporizadores y transporte. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define QEMU_CLOCK_VIRTUAL 0
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef uint64_t hwaddr;
typedef int *qemu_irq;
typedef struct ESP32C3GPIOState { int rutas[128]; } ESP32C3GPIOState;
typedef struct QEMUTimer {
    void (*cb)(void *);
    void *opaque;
    int64_t plazo;
    bool pendiente;
} QEMUTimer;
typedef struct GArray { uint32_t *data; unsigned len; } GArray;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
static int64_t ahora;
static int nivel_irq;
static int64_t qemu_clock_get_ns(int reloj) { return ahora; }
static void timer_mod_ns(QEMUTimer *t, int64_t plazo)
{
    assert(plazo >= ahora);
    t->plazo = plazo;
    t->pendiente = true;
}
static void timer_del(QEMUTimer *t) { t->pendiente = false; }
static void qemu_set_irq(qemu_irq irq, int nivel) { *irq = nivel; }
static void g_array_set_size(GArray *a, unsigned tamano) { a->len = tamano; }
static void anadir(GArray *a, uint32_t v)
{
    a->data = realloc(a->data, (a->len + 1) * sizeof(*a->data));
    assert(a->data);
    a->data[a->len++] = v;
}
#define g_array_append_val(a, v) anadir(a, v)
static int resolver_gpio(void *opaque, int senal)
{
    ESP32C3GPIOState *s = opaque;
    return s ? s->rutas[senal] : -1;
}
static bool taller_puente_activo(void) { return true; }
static struct {
    unsigned canal, gpio, tick, cantidad;
    int64_t inicio;
    uint32_t simbolos[2048];
} muestras[32];
static unsigned muestras_n;
static void taller_puente_rmt(uint8_t canal, uint8_t gpio, uint32_t tick,
                             const uint32_t *simbolos, size_t cantidad,
                             int64_t inicio)
{
    assert(muestras_n < 32 && cantidad < 2048);
    muestras[muestras_n].canal = canal;
    muestras[muestras_n].gpio = gpio;
    muestras[muestras_n].tick = tick;
    muestras[muestras_n].cantidad = (unsigned)cantidad;
    muestras[muestras_n].inicio = inicio;
    memcpy(muestras[muestras_n++].simbolos, simbolos, cantidad * 4);
}
/* CABECERA_REAL */
/* MODELO_REAL */
#ifdef MODELO_S3
#define CANALES_TX 4
#define SENAL0 81
#define REG_CONF 0x20
#define REG_ESTADO 0x50
#define REG_RAW 0x70
#define REG_ST 0x74
#define REG_ENA 0x78
#define REG_CLR 0x7c
#define REG_LIM 0xa0
#define REG_SYS 0xc0
#define REG_INVALIDO 0xd0
#define REG_RXCONF 0x30
#define RAM_BASE 0x800
#define RAM_FIN 0xe00
#else
#define CANALES_TX 2
#define SENAL0 51
#define REG_CONF 0x10
#define REG_ESTADO 0x28
#define REG_RAW 0x38
#define REG_ST 0x3c
#define REG_ENA 0x40
#define REG_CLR 0x44
#define REG_LIM 0x58
#define REG_SYS 0x68
#define REG_INVALIDO 0x74
#define REG_RXCONF 0x18
#define RAM_BASE 0x400
#define RAM_FIN 0x700
#endif
static ESP32C3RMTState rmt;
static ESP32C3GPIOState gpio;
static QEMUTimer timers[ESP32C3_RMT_CANALES_MAX];
static GArray arrays[ESP32C3_RMT_CANALES_MAX];
static void escribir(hwaddr a, uint32_t v) { esp32c3_rmt_write(&rmt, a, v, 4); }
static uint32_t leer(hwaddr a) { return (uint32_t)esp32c3_rmt_read(&rmt, a, 4); }
static void paso(unsigned canal)
{
    QEMUTimer *t = &timers[canal];
    assert(t->pendiente);
    ahora = t->plazo;
    t->pendiente = false;
    esp32c3_rmt_timer(&rmt.canal[canal]);
}
static void reiniciar(void)
{
    esp32c3_rmt_reiniciar(&rmt, 0);
    assert(!nivel_irq);
    for (unsigned n = 0; n < ESP32C3_RMT_CANALES_MAX; n++) {
        assert(!timers[n].pendiente);
    }
#ifdef MODELO_S3
    assert(leer(0xcc) == 0x02101181);
#else
    assert(leer(0xcc) == 0x02006231);
#endif
}
static uint32_t simbolo(bool uno)
{
    return uno ? 0x00048008u : 0x00088004u;
}
static unsigned codificar(uint32_t *destino, unsigned leds, unsigned grb)
{
    unsigned p = 0;
    for (unsigned l = 0; l < leds; l++) {
        for (int b = 23; b >= 0; b--) {
            destino[p++] = simbolo((grb >> b) & 1);
        }
    }
    destino[p++] = 0;
    return p;
}
/* Reproduce las recargas del encoder de IDF: escribe la mitad recién enviada.
 * Los datos futuros difieren: adelantar la lectura de RAM altera el tren. */
static void tren(unsigned canal, unsigned leds, unsigned grb, unsigned bloques)
{
    uint32_t esperado[1024];
    unsigned total = codificar(esperado, leds, grb);
    unsigned capacidad = 48 * bloques, mitad = capacidad / 2;
    unsigned siguiente = MIN(total, capacidad), recargas = 0;
    unsigned antes = muestras_n;
    int64_t inicio = ahora;
    unsigned base = RAM_BASE + canal * 48 * 4;
    for (unsigned i = 0; i < capacidad; i++) {
        escribir(base + 4 * i, i < total ? esperado[i] : 0);
    }
    escribir(REG_LIM + 4 * canal, mitad);
    escribir(REG_ENA, (1u << canal) | (1u << (8 + canal)));
    escribir(REG_CONF + 4 * canal, (bloques << 16) | (8 << 8) | 0x01000013);
    assert(!(leer(REG_CONF + 4 * canal) & 0x01800087u));
    assert(!muestras_n || muestras_n == antes);
    for (unsigned guardia = 0; muestras_n == antes; guardia++) {
        assert(guardia < 2000);
        paso(canal);
        if (leer(REG_RAW) & (1u << (8 + canal))) {
            if (leer(REG_RAW) & (1u << canal)) {
                break;
            }
            assert(nivel_irq && !timers[canal].pendiente);
            assert(leer(REG_ST) == (1u << (8 + canal)));
            /* Una ISR retrasada no debe consumir más memoria. */
            ahora += 50000;
            /* El driver nuevo limpia antes de llamar al encoder; también
             * cubrir el orden inverso que emplean otros drivers. */
            if (leds == 30) {
                escribir(REG_CLR, 1u << (8 + canal));
            }
            unsigned destino = (recargas++ & 1) ? mitad : 0;
            for (unsigned i = 0; i < mitad; i++) {
                escribir(base + 4 * (destino + i),
                         siguiente < total ? esperado[siguiente++] : 0);
            }
            if (leds != 30) {
                escribir(REG_CLR, 1u << (8 + canal));
            }
            assert(timers[canal].pendiente && !nivel_irq);
        }
    }
    assert(muestras_n == antes + 1 && recargas >= (total - 1) / mitad);
    assert(muestras[antes].canal == canal && muestras[antes].gpio == 8 + canal);
    assert(muestras[antes].tick == 100000 && muestras[antes].inicio == inicio);
    assert(muestras[antes].cantidad == total);
    assert(!memcmp(muestras[antes].simbolos, esperado, total * 4));
    assert(leer(REG_RAW) & (1u << canal));
    assert(leer(REG_ST) & (1u << canal));
    assert(nivel_irq && !timers[canal].pendiente);
    assert(ahora >= inicio + (int64_t)(total - 1) * 1200);
    escribir(REG_CLR, 0xffffffff);
    assert(!leer(REG_RAW) && !leer(REG_ST) && !nivel_irq && !leer(REG_CLR));
}
static void probar_tick(uint32_t reloj, uint32_t conf, unsigned esperado)
{
    reiniciar();
    escribir(REG_SYS, reloj | 1u | (1u << 26));
    escribir(RAM_BASE, 0x00018001);
    escribir(RAM_BASE + 4, 0);
    unsigned antes = muestras_n;
    escribir(REG_CONF, conf | 0x10003);
    paso(0);
    paso(0);
    assert(muestras_n == antes + 1 && muestras[antes].tick == esperado);
}
int main(void)
{
    memset(&gpio, 0xff, sizeof(gpio));
    for (unsigned n = 0; n < CANALES_TX; n++) {
        gpio.rutas[SENAL0 + n] = 8 + n;
    }
#ifdef MODELO_S3
    rmt.s3 = true;
#endif
    rmt.pin_de_senal = resolver_gpio;
    rmt.gpio = &gpio;
    rmt.irq = &nivel_irq;
    for (unsigned i = 0; i < ESP32C3_RMT_CANALES_MAX; i++) {
        rmt.canal[i].rmt = &rmt;
        rmt.canal[i].numero = i;
        rmt.canal[i].timer = &timers[i];
        rmt.canal[i].simbolos = &arrays[i];
    }
    reiniciar();
    /* FIFO no usado por IDF5, pero no debe pisar registros ni otros canales. */
    escribir(0, 0x12345678);
    escribir(REG_CONF, leer(REG_CONF) | 4);
    assert(leer(0) == 0x12345678);
    escribir(REG_SYS, 0x05000001); /* APB80MHz /1 y RAM directa. */
    tren(0, 8, 0x221133, 1);
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    tren(0, 30, 0x554466, 1);
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    tren(0, 8, 0x221133, 2); /* Canal0 toma los dos bloques TX. */
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    tren(1, 8, 0x221133, 1);
#ifdef MODELO_S3
    /* Los cuatro TX y los bloques prestados nunca invaden RX4..7. */
    for (unsigned n = 0; n < 4; n++) {
        reiniciar();
        escribir(REG_SYS, 0x05000001);
        tren(n, 30, 0x554466, 1);
    }
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    tren(0, 8, 0x221133, 4);
    reiniciar();
    escribir(REG_CONF + 12, (15u << 16) | (8u << 8));
    assert(esp32c3_rmt_capacidad(&rmt, 3) == 48);
    escribir(REG_CONF, (8u << 16) | (8u << 8));
    assert(esp32c3_rmt_capacidad(&rmt, 0) == 192);
#endif
    probar_tick(1u << 24, 8u << 8, 100000); /* APB80/8. */
    probar_tick((3u << 24) | (1u << 4), 2u << 8, 100000); /* XTAL40/2/2. */
    probar_tick((1u << 24) | (1u << 12) | (2u << 18), 4u << 8,
                75000); /* APB80 /1,5 /4. */
    probar_tick(2u << 24, 7u << 8, 400000); /* RC_FAST17,5 /7. */
#ifdef MODELO_S3
    /* Los cuatro TX y los bloques prestados nunca invaden RX4..7. */
    for (unsigned n = 0; n < 4; n++) {
        reiniciar();
        escribir(REG_SYS, 0x05000001);
        tren(n, 30, 0x554466, 1);
    }
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    tren(0, 8, 0x221133, 4);
    reiniciar();
    escribir(REG_CONF + 12, (15u << 16) | (8u << 8));
    assert(esp32c3_rmt_capacidad(&rmt, 3) == 48);
    escribir(REG_CONF, (8u << 16) | (8u << 8));
    assert(esp32c3_rmt_capacidad(&rmt, 0) == 192);
#endif
    probar_tick(1u << 24, 0, 3200000); /* Canal divisor0 significa256. */
    /* ST siempre raw&ena; RAW y STATUS no son escribibles. */
    assert(leer(REG_RAW) == 1 && !leer(REG_ST) && !nivel_irq);
    escribir(REG_RAW, 0);
    escribir(REG_ST, 0xffffffff);
    escribir(REG_ESTADO, 0xffffffff);
    assert(leer(REG_RAW) == 1 && !leer(REG_ST));
    escribir(REG_ENA, 1);
    assert(leer(REG_ST) == 1 && nivel_irq);
    escribir(REG_ENA, 0);
    assert(!leer(REG_ST) && !nivel_irq);
    /* No se precarga el siguiente símbolo antes del fin del anterior. */
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    escribir(RAM_BASE, simbolo(false));
    escribir(RAM_BASE + 4, simbolo(false));
    escribir(RAM_BASE + 8, 0);
    unsigned antes = muestras_n;
    escribir(REG_CONF, 0x10803);
    escribir(RAM_BASE + 4, simbolo(true));
    paso(0);
    paso(0);
    paso(0);
    assert(muestras_n == antes + 1 && muestras[antes].simbolos[1] == 0x00048008);
    /* Deshabilitar la IRQ de umbral también libera la contrapresión. */
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    escribir(RAM_BASE, simbolo(false));
    escribir(RAM_BASE + 4, simbolo(true));
    escribir(RAM_BASE + 8, 0);
    escribir(REG_LIM, 1);
    escribir(REG_ENA, 0x100);
    antes = muestras_n;
    escribir(REG_CONF, 0x10803);
    paso(0);
    assert(nivel_irq && !timers[0].pendiente);
    escribir(REG_ENA, 0);
    assert(!nivel_irq && timers[0].pendiente && leer(REG_RAW) == 0x100);
    paso(0);
    paso(0);
    assert(muestras_n == antes + 1 && leer(REG_RAW) == 0x101 && !leer(REG_ST));
    /* Un semisímbolo con duración1=0 transmite duración0 y termina. */
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    escribir(RAM_BASE, 0x00008004);
    antes = muestras_n;
    int64_t inicio = ahora;
    escribir(REG_CONF, 0x10803);
    paso(0);
    assert(muestras_n == antes + 1 && muestras[antes].cantidad == 1);
    assert(muestras[antes].simbolos[0] == 0x00008004 && ahora == inicio + 400);
    /* Reset y stop cancelan el timer y descartan trenes incompletos. */
    escribir(RAM_BASE, simbolo(false));
    escribir(REG_CONF, 0x10803);
    assert(timers[0].pendiente);
    reiniciar();
    assert(muestras_n == antes + 1 && !leer(RAM_BASE));
    escribir(REG_SYS, 0x05000001);
    escribir(RAM_BASE, simbolo(false));
    escribir(REG_CONF, 0x10803);
    escribir(REG_CONF, 0x01010880);
    assert(!timers[0].pendiente && !(leer(REG_CONF) & 0x01800087));
    assert(muestras_n == antes + 1);
    /* Reset durante la contrapresión borra el evento sin reanudar el TX. */
    escribir(REG_LIM, 1);
    escribir(REG_ENA, 0x100);
    escribir(REG_CONF, 0x10803);
    paso(0);
    assert(nivel_irq && !timers[0].pendiente);
    reiniciar();
    escribir(REG_CLR, 0x100);
    assert(!timers[0].pendiente && muestras_n == antes + 1);
    /* Sin fuente, reloj parado o MEM_SIZE=0: error y ningún símbolo emitido. */
    escribir(REG_SYS, 1);
    escribir(REG_CONF, 0x10803);
    assert(leer(REG_RAW) == 0x10 && !timers[0].pendiente);
    reiniciar();
    escribir(REG_SYS, 0x01000001);
    escribir(REG_CONF, 0x10803);
    assert(leer(REG_RAW) == 0x10 && !timers[0].pendiente);
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    escribir(REG_CONF, 0x803);
    assert(leer(REG_RAW) == 0x10 && !timers[0].pendiente);
    /* Memoria inexistente, direcciones no alineadas, RX sin TX implícito. */
    escribir(RAM_FIN, 0xffffffff);
    escribir(0x1000, 0xffffffff);
    escribir(RAM_BASE + 1, 0xffffffff);
    escribir(REG_INVALIDO, 0xffffffff);
    escribir(REG_RXCONF, 0xffffffff);
    assert(!leer(RAM_FIN) && !leer(0x1000) && !leer(RAM_BASE + 1) && !leer(REG_INVALIDO));
    assert(!timers[0].pendiente && !timers[1].pendiente);
#ifdef MODELO_S3
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    gpio.rutas[84] = 48;
    escribir(RAM_BASE + 3 * 48 * 4, simbolo(false));
    escribir(RAM_BASE + 3 * 48 * 4 + 4, 0);
    escribir(REG_CONF + 12, 0x10803);
    assert(leer(REG_ESTADO + 12) == (144u | (1u << 22)));
    antes = muestras_n;
    paso(3);
    paso(3);
    assert(muestras_n == antes + 1 && muestras[antes].canal == 3);
    assert(muestras[antes].gpio == 48 && muestras[antes].cantidad == 2);
    assert(leer(REG_RAW) == 8);
    escribir(RAM_FIN - 4, 0x12345678);
    assert(leer(RAM_FIN - 4) == 0x12345678 && !leer(RAM_FIN));
    /* Configurar RX y sus FIFO jamás arranca un TX. */
    escribir(REG_RXCONF + 4, 0xffffffff);
    assert(!timers[0].pendiente && !timers[3].pendiente);
    reiniciar();
    escribir(REG_SYS, 0x05000001);
    escribir(RAM_BASE, simbolo(false));
    escribir(RAM_BASE + 4, 0);
    escribir(RAM_BASE + 3 * 48 * 4, simbolo(true));
    escribir(RAM_BASE + 3 * 48 * 4 + 4, 0);
    escribir(REG_CONF, 0x10803);
    escribir(REG_CONF + 12, 0x10803);
    antes = muestras_n;
    paso(0); paso(3); paso(0); paso(3);
    assert(muestras_n == antes + 2 && leer(REG_RAW) == 9);
#endif
    for (unsigned i = 0; i < ESP32C3_RMT_CANALES_MAX; i++) {
        free(arrays[i].data);
    }
#ifdef MODELO_S3
    puts("OK RMT S3 real: cuatro TX, GPIO48, bloques TX/RX, ping-pong y reset");
#else
    puts("OK RMT C3 real: 8/30 LED, ping-pong, IRQ, dos TX, RAM, relojes y reset");
#endif
    return 0;
}
