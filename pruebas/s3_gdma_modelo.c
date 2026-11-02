/* Adaptadores mínimos del traductor GDMA S3 real; IRQ por dirección y canal. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _MSC_VER
/* MSVC avisa de enums anónimos incluso tras los casts int del código base. */
#pragma warning(disable: 5287)
#endif
typedef uint64_t hwaddr;
#define ESP_GDMA_IN_IDX 0
#define ESP_GDMA_OUT_IDX 1
#define ESP32S3_GDMA(obj) ((ESP32S3GdmaState *)(obj))
#define REG32(reg, addr) enum { A_##reg = addr, R_##reg = addr / 4 };
#define FIELD(reg, nombre, bit, ancho) \
    enum { R_##reg##_##nombre##_MASK = ((UINT64_C(1) << ancho) - 1) << bit };
typedef enum {
    GDMA_UNKNOWN=-1, GDMA_CONF0_REG, GDMA_CONF1_REG, GDMA_INT_RAW_REG,
    GDMA_INT_ST_REG, GDMA_INT_ENA_REG, GDMA_INT_CLR_REG, GDMA_FIFO_ST_REG,
    GDMA_POP_REG, GDMA_LINK_REG, GDMA_STATE_REG, GDMA_SUC_EOF_DESC_REG,
    GDMA_ERR_EOF_DESC_REG, GDMA_DESC_ADDR_REG, GDMA_BF0_DESC_ADDR_REG,
    GDMA_BF1_DESC_ADDR_REG, GDMA_DUMMY_3C_REG, GDMA_DUMMY_40_REG,
    GDMA_PRIORITY_REG, GDMA_PERI_SEL_REG, GDMA_MISC_REG, NUM_REGS
} DmaRegister;
/* Eventos genéricos de esp_gdma.h: referencias independientes del traductor. */
FIELD(GDMA_INTERRUPT, IN_DSCR_EMPTY, 4, 1)
FIELD(GDMA_INTERRUPT, IN_DSCR_ERR, 3, 1)
FIELD(GDMA_INTERRUPT, IN_ERR_EOF, 2, 1)
FIELD(GDMA_INTERRUPT, IN_SUC_EOF, 1, 1)
FIELD(GDMA_INTERRUPT, IN_DONE, 0, 1)
FIELD(GDMA_INTERRUPT, OUT_TOTAL_EOF, 3, 1)
FIELD(GDMA_INTERRUPT, OUT_DSCR_ERR, 2, 1)
FIELD(GDMA_INTERRUPT, OUT_EOF, 1, 1)
FIELD(GDMA_INTERRUPT, OUT_DONE, 0, 1)
/* CABECERA_REAL */
typedef struct { uint32_t regs[2][5][NUM_REGS]; } ESPGdmaState;
typedef struct { ESPGdmaState parent; } ESP32S3GdmaState;
static uint64_t esp_gdma_read_chan_register(ESPGdmaState *s, unsigned d, unsigned c, DmaRegister r)
{
    assert(d < 2 && c < 5 && r >= 0 && r < NUM_REGS);
    return s->regs[d][c][r];
}
static void esp_gdma_write_chan_register(ESPGdmaState *s, unsigned d, unsigned c, DmaRegister r, uint32_t v)
{
    assert(d < 2 && c < 5 && r >= 0 && r < NUM_REGS);
    if (r == GDMA_INT_CLR_REG || r == GDMA_INT_RAW_REG) {
        s->regs[d][c][GDMA_INT_RAW_REG] &= ~v;
    } else if (r != GDMA_INT_ST_REG) { s->regs[d][c][r] = v; }
    s->regs[d][c][GDMA_INT_ST_REG] = s->regs[d][c][GDMA_INT_RAW_REG] & s->regs[d][c][GDMA_INT_ENA_REG];
}
static uint64_t esp_gdma_read_register(ESPGdmaState *s, DmaRegister r) { return s->regs[0][0][r]; }
static void esp_gdma_write_register(ESPGdmaState *s, DmaRegister r, uint32_t v) { s->regs[0][0][r] = v; }
/* MODELO_REAL */
int main(void)
{
    ESP32S3GdmaState s = {0};
    for (unsigned c = 0; c < 5; c++) {
        for (unsigned d = 0; d < 2; d++) {
            unsigned base = c * 0xc0 + d * 0x60;
            for (unsigned bit = 0; bit < (d ? 8u : 10u); bit++) {
                uint32_t v = 1u << bit;
                s.parent.regs[d][c][GDMA_INT_RAW_REG] = v;
                esp32s3_gdma_write(&s, base + 0x10, v, 4);
                assert(esp32s3_gdma_read(&s, base + 8, 4) == v);
                assert(esp32s3_gdma_read(&s, base + 12, 4) == v);
                esp32s3_gdma_write(&s, base + 0x14, v, 4);
                assert(!esp32s3_gdma_read(&s, base + 8, 4));
            }
            esp32s3_gdma_write(&s, base + 0x48, c + d, 4);
            assert(esp32s3_gdma_read(&s, base + 0x48, 4) == c + d);
            unsigned start = d ? 21 : 22, stop = d ? 20 : 21, park = d ? 23 : 24;
            uint32_t dir = 0x88100u | (d ? 0 : 1u << 20); /* AUTO_RET IN */
            esp32s3_gdma_write(&s, base + 0x20, dir | (1u << start), 4);
            esp32s3_gdma_write(&s, base + 0x20, dir | (1u << stop), 4);
            assert(esp32s3_gdma_read(&s, base + 0x20, 4) == (dir | (1u << park)));
            esp32s3_gdma_write(&s, base + 0x20, dir | (1u << start) | (1u << park), 4);
            assert(esp32s3_gdma_read(&s, base + 0x20, 4) == (dir | (1u << start)));
            esp32s3_gdma_write(&s, base, 1, 4);
            esp32s3_gdma_write(&s, base, 0, 4);
            assert(esp32s3_gdma_read(&s, base + 0x20, 4) == (dir | (1u << park)));
        }
        /* Limpiar OUT no modifica IN del mismo canal. */
        s.parent.regs[0][c][GDMA_INT_RAW_REG] = 3;
        s.parent.regs[1][c][GDMA_INT_RAW_REG] = 3;
        esp32s3_gdma_write(&s, c * 0xc0 + 0x74, 3, 4);
        assert(esp32s3_gdma_read(&s, c * 0xc0 + 8, 4) == 3);
        assert(!esp32s3_gdma_read(&s, c * 0xc0 + 0x68, 4));
    }
    assert(!esp32s3_gdma_read(&s, 0x3c0, 4));
    assert(!esp32s3_gdma_read(&s, 0x21, 4));
    puts("OK GDMA S3 real: cinco canales, mapa nativo, IRQ separadas, STOP/START/reset");
    return 0;
}
