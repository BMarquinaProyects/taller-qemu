"""Mapa GDMA S3 real, con cabeceras QEMU reales y pulsos de reutilización."""
import re
import shutil
import unittest
from pathlib import Path

from c3_modelos import compilar_ejecutar


class PruebaGdmaS3(unittest.TestCase):
    def test_mapa_real_cinco_canales_irq_stop_y_reset(self):
        raiz = Path(__file__).resolve().parents[1]
        fuente = raiz / "capas/hw/dma/esp32s3_gdma.c"
        self.assertTrue(fuente.exists(), "falta capa GDMA S3 con pulsos STOP/reset")
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or shutil.which("cl")
        if not cc:
            self.skipTest("compilador C no disponible")
        # Las cabeceras se incluyen en capas para reproducir el banco sin WSL.
        cab = (raiz / "capas/include/hw/dma/esp32s3_gdma.h").read_text(encoding="utf-8")
        canales = re.search(r"^#define ESP32S3_GDMA_CHANNEL_COUNT[^\n]+", cab, re.M).group()
        cab = canales + "\n" + cab[cab.index("#define DMA_DIR_REGS_SIZE"):]
        codigo = fuente.read_text(encoding="utf-8")
        codigo = codigo[codigo.index("/* For now"):codigo.index("static const MemoryRegionOps")]
        codigo = re.sub(r"#if ESP32S3_GDMA_(?:WARNING|DEBUG).*?#endif", "", codigo, flags=re.S)
        plantilla = (raiz / "pruebas/s3_gdma_modelo.c").read_text(encoding="utf-8")
        compilar_ejecutar(plantilla.replace("/* CABECERA_REAL */", cab).replace(
            "/* MODELO_REAL */", codigo), cc)
