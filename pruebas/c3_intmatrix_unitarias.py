"""Matriz de interrupciones del C3: las fuentes 32..61 no pueden pisar a las 0..29 en Windows.

En Windows (LLP64) `long` tiene 32 bits y el BIT() de QEMU es (1UL << n): para n >= 32 el
desplazamiento es indefinido y en x86 cuenta módulo 32. Esta prueba compila los macros y la
función de nivel reales con un BIT() que imita eso; si el código vuelve a usar BIT(), el I2C0
(fuente 29) enciende la línea de CACHE_CORE0_ACS (61) y falla."""
from pathlib import Path
import shutil
import unittest

from c3_modelos import compilar_ejecutar

PLANTILLA = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define ESP32C3_INT_MATRIX_INPUTS 62
/* Como Windows x86: `1UL` de 32 bits y cuenta de desplazamiento módulo 32. */
#define BIT(nr) ((uint32_t)1u << ((nr) & 31))
#define BIT_ULL(nr) (1ULL << (nr))
typedef struct { uint8_t irq_map[ESP32C3_INT_MATRIX_INPUTS]; uint64_t irq_levels; } ESP32C3IntMatrixState;
/* MODELO_REAL */
int main(void)
{
    ESP32C3IntMatrixState s = {0};
    s.irq_map[29] = 3;   /* I2C0 → línea 3 */
    s.irq_map[61] = 25;  /* CACHE_CORE0_ACS → línea 25 (error de caché en IDF) */
    SET_BIT(s.irq_levels, 29);
    assert(esp32c3_get_output_line_level(&s, 3) == 1);
    assert(esp32c3_get_output_line_level(&s, 25) == 0);
    CLEAR_BIT(s.irq_levels, 29);
    SET_BIT(s.irq_levels, 61);
    assert(BIT_SET(s.irq_levels, 61) && !BIT_SET(s.irq_levels, 29));
    assert(esp32c3_get_output_line_level(&s, 25) == 1);
    assert(esp32c3_get_output_line_level(&s, 3) == 0);
    puts("OK matriz C3: fuentes 29 y 61 independientes");
    return 0;
}
"""


class PruebaMatrizC3(unittest.TestCase):
    def test_fuentes_altas_no_pisan_a_las_bajas(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or shutil.which("cl")
        if not cc:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        codigo = (raiz / "capas/hw/riscv/esp32c3_intmatrix.c").read_text(encoding="utf-8")
        macros = codigo[codigo.index("#define BIT_SET"):codigo.index("static int esp32c3_get_output_line_level")]
        inicio = codigo.index("static int esp32c3_get_output_line_level")
        funcion = codigo[inicio:codigo.index("\n}\n", inicio) + 3]
        compilar_ejecutar(PLANTILLA.replace("/* MODELO_REAL */", macros + funcion), cc)


if __name__ == "__main__":
    unittest.main()
