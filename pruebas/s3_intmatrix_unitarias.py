"""Matriz S3 real: niveles pendientes, OR y reasignación en ambas CPU.

Compila cabecera y lógica MMIO/IRQ/reset reales con dobles mínimos de CPU
y líneas QEMU. BIT() imita LLP64 para detectar regresiones de Windows.
"""
from pathlib import Path
import shutil
import unittest

from c3_modelos import compilar_ejecutar


PLANTILLA = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP32S3_CPU_COUNT 2
#define BIT(n) ((uint32_t)1u << ((n) & 31))
#define BIT_ULL(n) (1ULL << (n))
typedef uint64_t hwaddr;
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef void Object;
typedef int ResetType;
typedef int *qemu_irq;
typedef struct { int nextint; int extint[4]; } XtensaConfig;
typedef struct { struct { const XtensaConfig *config; } env; } XtensaCPU;
#define OBJECT_CHECK(tipo, obj, nombre) ((tipo *)(obj))
static void qemu_set_irq(qemu_irq irq, int nivel) { *irq = nivel != 0; }
static void qemu_irq_lower(qemu_irq irq) { qemu_set_irq(irq, 0); }
/* CABECERA_REAL */
/* MODELO_REAL */
static void asignar(Esp32s3IntMatrixState *s, int cpu, int fuente, int linea)
{
    hwaddr dir = (cpu * ESP32S3_INT_MATRIX_INPUTS + fuente) * sizeof(uint32_t);
    esp32s3_intmatrix_write(s, dir, linea, 4);
    assert(esp32s3_intmatrix_read(s, dir, 4) == (uint64_t)linea);
}
int main(void)
{
    /* Órdenes distintos: comprobar número de línea, no índice de extint. */
    const XtensaConfig config[2] = {{4, {25, 3, 9, 1}}, {4, {9, 1, 25, 3}}};
    XtensaCPU cpu[2] = {{{&config[0]}}, {{&config[1]}}};
    Esp32s3IntMatrixState s = {0};
    int niveles[2][32] = {{0}};
    qemu_irq salidas[2][4];
    for (int c = 0; c < 2; c++) {
        s.cpu[c] = &cpu[c];
        s.outputs[c] = salidas[c];
        for (int i = 0; i < 4; i++) {
            salidas[c][i] = &niveles[c][config[c].extint[i]];
        }
    }
    esp32s3_intmatrix_reset_hold((Object *)&s, 0);
    /* CASO */
    puts("OK matriz S3");
    return 0;
}
"""


class PruebaMatrizS3(unittest.TestCase):
    def compilar(self, caso):
        cc = (shutil.which("cc") or shutil.which("gcc") or
              shutil.which("clang") or shutil.which("cl"))
        if not cc:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        cab = (raiz / "capas/include/hw/xtensa/esp32s3_intc.h").read_text(encoding="utf-8")
        cab = "\n".join(l for l in cab.splitlines()
                        if not l.startswith(("#include", "#pragma")))
        codigo = (raiz / "capas/hw/xtensa/esp32s3_intc.c").read_text(encoding="utf-8")
        logica = codigo[codigo.index("#define INTMATRIX_UNINT_VALUE"):
                        codigo.index("static const MemoryRegionOps")]
        reset = codigo[codigo.index("static void esp32s3_intmatrix_reset_hold"):
                       codigo.index("static void esp32s3_intmatrix_realize")]
        compilar_ejecutar(PLANTILLA.replace("/* CABECERA_REAL */", cab).replace(
            "/* MODELO_REAL */", logica + reset).replace("/* CASO */", caso), cc)

    def test_alta_antes_de_asignar_despierta_ambas_cpu(self):
        self.compilar(r"""
        esp32s3_intmatrix_irq_handler(&s, 21, 1); /* SPI2 trans_done pendiente. */
        assert(!niveles[0][3] && !niveles[1][9]);
        asignar(&s, 0, 21, 3);
        assert(niveles[0][3] && !niveles[1][9]);
        asignar(&s, 1, 21, 9);
        assert(niveles[0][3] && niveles[1][9]);
        """)

    def test_bajar_fuente_compartida_conserva_or(self):
        self.compilar(r"""
        for (int c = 0; c < 2; c++) {
            asignar(&s, c, 21, 3);
            asignar(&s, c, 42, 3);
        }
        esp32s3_intmatrix_irq_handler(&s, 21, 1);
        esp32s3_intmatrix_irq_handler(&s, 42, 1);
        esp32s3_intmatrix_irq_handler(&s, 21, 0);
        assert(niveles[0][3] && niveles[1][3]);
        esp32s3_intmatrix_irq_handler(&s, 42, 0);
        assert(!niveles[0][3] && !niveles[1][3]);
        """)

    def test_reasignar_apaga_vieja_enciende_nueva_sin_afectar_otra_cpu(self):
        self.compilar(r"""
        asignar(&s, 0, 21, 3);
        asignar(&s, 1, 21, 25);
        esp32s3_intmatrix_irq_handler(&s, 21, 1);
        asignar(&s, 0, 21, 9);
        assert(!niveles[0][3] && niveles[0][9] && niveles[1][25]);
        asignar(&s, 1, 21, 1);
        assert(!niveles[1][25] && niveles[1][1] && niveles[0][9]);
        asignar(&s, 0, 21, 6); /* esp_intr_disable. */
        assert(!niveles[0][9] && niveles[1][1]);
        asignar(&s, 0, 21, 9); /* esp_intr_enable, sin otro flanco. */
        assert(niveles[0][9] && niveles[1][1]);
        """)

    def test_reasignar_conserva_otra_fuente_en_linea_vieja(self):
        self.compilar(r"""
        asignar(&s, 0, 21, 3);
        asignar(&s, 0, 42, 3);
        esp32s3_intmatrix_irq_handler(&s, 21, 1);
        esp32s3_intmatrix_irq_handler(&s, 42, 1);
        asignar(&s, 0, 21, 9);
        assert(niveles[0][3] && niveles[0][9]);
        esp32s3_intmatrix_irq_handler(&s, 42, 0);
        assert(!niveles[0][3] && niveles[0][9]);
        """)

    def test_fuentes_29_61_y_90_mas_independientes(self):
        self.compilar(r"""
        const int fuentes[] = {26, 29, 58, 61, 90, 93, 98,
                               ESP32S3_INT_MATRIX_INPUTS - 1};
        for (unsigned i = 0; i < sizeof(fuentes) / sizeof(fuentes[0]); i++) {
            int alta = fuentes[i];
            esp32s3_intmatrix_reset_hold((Object *)&s, 0);
            esp32s3_intmatrix_irq_handler(&s, alta, 1);
            for (unsigned j = 0; j < sizeof(fuentes) / sizeof(fuentes[0]); j++) {
                for (int c = 0; c < 2; c++) {
                    asignar(&s, c, fuentes[j], alta == fuentes[j] ? 3 : 25);
                }
            }
            assert(niveles[0][3] && niveles[1][3]);
            assert(!niveles[0][25] && !niveles[1][25]);
            esp32s3_intmatrix_irq_handler(&s, alta, 0);
            assert(!niveles[0][3] && !niveles[1][3]);
        }
        """)

    def test_reset_borra_niveles_y_cpu_sin_salida_conserva_pendiente(self):
        self.compilar(r"""
        s.outputs[1] = NULL;
        esp32s3_intmatrix_irq_handler(&s, 98, 1);
        s.outputs[1] = salidas[1];
        asignar(&s, 1, 98, 25);
        assert(niveles[1][25]);
        esp32s3_intmatrix_reset_hold((Object *)&s, 0);
        assert(!niveles[1][25]);
        asignar(&s, 0, 98, 3);
        asignar(&s, 1, 98, 25);
        assert(!niveles[0][3] && !niveles[1][25]);
        """)


if __name__ == "__main__":
    unittest.main()
