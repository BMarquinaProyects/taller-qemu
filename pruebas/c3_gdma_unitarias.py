"""Traducción real del GDMA C3: IRQ IN/OUT y registros de los tres canales."""
from pathlib import Path
import shutil
import unittest

from c3_modelos import compilar_ejecutar


class PruebaGdmaC3(unittest.TestCase):
    def test_traduccion_real_canales_y_todos_los_bits_irq(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or shutil.which("cl")
        if not cc:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        fuente = raiz / "capas/hw/dma/esp32c3_gdma.c"
        self.assertTrue(fuente.exists(), "falta corrección de traducción GDMA C3")
        codigo = fuente.read_text(encoding="utf-8")
        codigo = codigo[codigo.index("/* Traducción"):codigo.index("static const MemoryRegionOps")]
        plantilla = (raiz / "pruebas/c3_gdma_modelo.c").read_text(encoding="utf-8")
        compilar_ejecutar(plantilla.replace("/* MODELO_REAL */", codigo), cc)
