/* Adaptadores para ejecutar la traducción real sin registrar QOM. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t hwaddr;
#define ESP_GDMA_IN_IDX 0
#define ESP_GDMA_OUT_IDX 1
#define DMA_DIR_REGS_SIZE 0x60
#define DMA_INT_CHAN_REGS_SIZE 0x10
#define ESP32C3_GDMA(obj) ((ESP32C3GdmaState *)(obj))
typedef enum {
    GDMA_UNKNOWN=-1, GDMA_CONF0_REG, GDMA_CONF1_REG, GDMA_INT_RAW_REG,
    GDMA_INT_ST_REG, GDMA_INT_ENA_REG, GDMA_INT_CLR_REG, GDMA_FIFO_ST_REG,
    GDMA_POP_REG, GDMA_LINK_REG, GDMA_STATE_REG, GDMA_SUC_EOF_DESC_REG,
    GDMA_ERR_EOF_DESC_REG, GDMA_DESC_ADDR_REG, GDMA_BF0_DESC_ADDR_REG,
    GDMA_BF1_DESC_ADDR_REG, GDMA_PRIORITY_REG, GDMA_PERI_SEL_REG, GDMA_MISC_REG,
    NUM_REGS
} DmaRegister;
typedef struct { uint32_t regs[2][3][NUM_REGS]; } ESPGdmaState;
typedef struct { ESPGdmaState parent; } ESP32C3GdmaState;
static uint32_t esp_gdma_read_chan_register(ESPGdmaState *s, unsigned d, unsigned c, DmaRegister r)
{
    assert(d < 2 && c < 3 && r >= 0 && r < NUM_REGS);
    return s->regs[d][c][r];
}
static void esp_gdma_write_chan_register(ESPGdmaState *s, unsigned d, unsigned c, DmaRegister r, uint32_t v)
{
    assert(d < 2 && c < 3 && r >= 0 && r < NUM_REGS);
    if (r == GDMA_INT_CLR_REG || r == GDMA_INT_RAW_REG) {
        s->regs[d][c][GDMA_INT_RAW_REG] &= ~v;
    } else { s->regs[d][c][r] = v; }
    s->regs[d][c][GDMA_INT_ST_REG] = s->regs[d][c][GDMA_INT_RAW_REG] & s->regs[d][c][GDMA_INT_ENA_REG];
}
static uint32_t esp_gdma_read_register(ESPGdmaState *s, DmaRegister r) { return s->regs[0][0][r]; }
static void esp_gdma_write_register(ESPGdmaState *s, DmaRegister r, uint32_t v) { s->regs[0][0][r] = v; }
/* MODELO_REAL */
int main(void)
{
    ESP32C3GdmaState s = {0};
    for (unsigned c = 0; c < 3; c++) {
        for (unsigned bit = 0; bit < 9; bit++) {
            unsigned dir = bit < 3 || bit == 5 || bit == 7 ? 0 : 1;
            const unsigned mapa[] = {0,1,2,0,1,3,2,4,3};
            uint32_t generico = 1u << mapa[bit];
            s.parent.regs[dir][c][GDMA_INT_RAW_REG] = generico;
            esp32c3_gdma_write(&s, c * 16 + 8, UINT64_C(1) << bit, 4);
            assert(s.parent.regs[dir][c][GDMA_INT_ENA_REG] == generico);
            assert(s.parent.regs[!dir][c][GDMA_INT_ENA_REG] == 0);
            assert(esp32c3_gdma_read(&s, c * 16, 4) == (UINT64_C(1) << bit));
            assert(esp32c3_gdma_read(&s, c * 16 + 4, 4) == (UINT64_C(1) << bit));
            esp32c3_gdma_write(&s, c * 16 + 12, UINT64_C(1) << bit, 4);
            assert(!esp32c3_gdma_read(&s, c * 16, 4));
        }
        /* Una IRQ RX pendiente sobrevive al borrado de TX y viceversa. */
        s.parent.regs[0][c][GDMA_INT_RAW_REG] = 3;
        s.parent.regs[1][c][GDMA_INT_RAW_REG] = 3;
        esp32c3_gdma_write(&s, c * 16 + 8, 0x1ff, 4);
        esp32c3_gdma_write(&s, c * 16 + 12, 0x18, 4);
        assert(esp32c3_gdma_read(&s, c * 16 + 4, 4) == 3);
        esp32c3_gdma_write(&s, c * 16 + 12, 3, 4);
        assert(!esp32c3_gdma_read(&s, c * 16 + 4, 4));
        for (unsigned d = 0; d < 2; d++) {
            unsigned base = 0x70 + c * 0xc0 + d * 0x60;
            esp32c3_gdma_write(&s, base + 0x30, 7 + c + d, 4);
            assert(esp32c3_gdma_read(&s, base + 0x30, 4) == 7 + c + d);
            esp32c3_gdma_write(&s, base + 0x10, 0x12345, 4);
            assert(esp32c3_gdma_read(&s, base + 0x10, 4) == 0x12345);
            unsigned start = d ? 21 : 22, stop = d ? 20 : 21, park = d ? 23 : 24;
            esp32c3_gdma_write(&s, base + 0x10, 0x12345u | (UINT64_C(1) << start), 4);
            esp32c3_gdma_write(&s, base + 0x10, 0x12345u | (UINT64_C(1) << stop), 4);
            assert(esp32c3_gdma_read(&s, base + 0x10, 4) == (0x12345u | (UINT64_C(1) << park)));
            esp32c3_gdma_write(&s, base + 0x10, 0x12345u | (UINT64_C(1) << start), 4);
            esp32c3_gdma_write(&s, base, 1, 4);
            esp32c3_gdma_write(&s, base, 0, 4);
            assert(!(esp32c3_gdma_read(&s, base + 0x10, 4) & (UINT64_C(1) << start)));
        }
    }
    assert(!esp32c3_gdma_read(&s, 0x1000, 4));
    assert(!esp32c3_gdma_read(&s, 0x71, 4));
    puts("OK GDMA C3 real: stride IRQ 0/1/2, nueve eventos, IN/OUT, ENA/CLR y registros");
    return 0;
}
