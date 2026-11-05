"""Modelos reales compartidos: I2C0 S3 y RMT con mapa S3 de cuatro TX."""
import unittest
import shutil
from pathlib import Path

from c3_modelos import ejecutar_modelo, compilar_ejecutar


class PruebaModelosI2cRmtS3(unittest.TestCase):
    def test_i2c_fifo_comandos_nack_espejo_y_reset(self):
        ejecutar_modelo(self, "i2c", carpeta="i2c", s3=True)

    def test_rmt_cuatro_tx_ping_pong_gpio48_relojes_y_reset(self):
        ejecutar_modelo(self, "rmt", s3=True)

    def test_receptor_real_con_parser_fragmentado_altas_y_registros_bus0(self):
        cc = (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or
              shutil.which("cl"))
        if cc is None:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        codigo = (raiz / "pruebas/armado_receptor.c").read_text(encoding="utf-8")
        archivos = {
            "clasico_receptor.h": ("xtensa/esp32.c", "esp32_taller_recibir", "esp32_soc_realize"),
            "c3_receptor.h": ("riscv/esp32c3.c", "esp32c3_taller_recibir", "esp32c3_machine_init"),
            "s3_receptor.h": ("xtensa/esp32s3.c", "esp32s3_taller_recibir", "esp32s3_machine_init"),
        }
        for cabecera, (archivo, inicio, fin) in archivos.items():
            fuente = (raiz / "capas/hw" / archivo).read_text(encoding="utf-8")
            receptor = fuente[fuente.index(f"static void {inicio}("):
                              fuente.index(f"static void {fin}(")]
            codigo = codigo.replace(f'#include "{cabecera}"', receptor)
        puente = (raiz / "capas/hw/misc/taller_puente.c").read_text(encoding="utf-8")
        codigo = codigo.replace('#include "puente_receptor.h"',
                                puente[puente.index("/* Tramas que llegan"):
                                       puente.index("/* Por bus:")])
        armado = (raiz / "capas/include/hw/misc/taller_armado.h").as_posix()
        codigo = codigo.replace('"hw/misc/taller_armado.h"', f'"{armado}"')
        tren = (raiz / "capas/include/hw/misc/taller_tren.h").as_posix()
        codigo = codigo.replace('"hw/misc/taller_tren.h"', f'"{tren}"')
        unhilo = (raiz / "capas/include/hw/misc/taller_unhilo.h").as_posix()
        codigo = codigo.replace('"hw/misc/taller_unhilo.h"', f'"{unhilo}"')
        compilar_ejecutar(codigo, cc)


if __name__ == "__main__":
    unittest.main()
