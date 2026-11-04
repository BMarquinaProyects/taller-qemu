"""ENTRADA_TREN (v8): lógica C real y constructor/verificador del modo tren; no necesitan QEMU."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import qemu_prueba


class PruebaTren(unittest.TestCase):
    def test_logica_c(self):
        cc = (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or
              shutil.which("cl"))
        if cc is None:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        fuente = raiz / "pruebas" / "tren_logica.c"
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "tren.exe"
            if Path(cc).stem.lower() == "cl":
                comando = [cc, "/nologo", "/std:c11", "/W4", "/WX", "/wd4100",
                           "/wd4244", "/wd4267", "/utf-8", f"/I{raiz / 'capas/include'}",
                           str(fuente), f"/Fe:{exe}", f"/Fo:{Path(tmp) / 'tren.obj'}"]
            else:
                comando = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           "-I", str(raiz / "capas/include"), str(fuente), "-o", str(exe)]
            subprocess.run(comando, check=True)
            subprocess.run([str(exe)], check=True)

    def test_constructor_sin_relleno(self):
        t = qemu_prueba.trama_entrada_tren(16, [(0, 0), (300000, 2), (300000, 1)])
        self.assertEqual(t[:4], struct.pack("<BBH", 21, 8, 18))
        self.assertEqual(t[12:], bytes([16, 3, 0]) + struct.pack("<IB", 0, 0) +
                         struct.pack("<IB", 300000, 2) + struct.pack("<IB", 300000, 1))
        with self.assertRaises(ValueError):
            qemu_prueba.trama_entrada_tren(16, [(0, 0)] * 65)
        with self.assertRaises(ValueError):
            qemu_prueba.trama_entrada_tren(16, [(0, 3)])

    def test_verificador(self):
        bien = ("TREN-LISTO\r\nTREN-INICIAL=1\r\nFLANCOS=4\r\nNIVEL=0\r\n"
                "TREN-CANCELA-LISTO\r\nCANCELADO=1\r\nTALLER-OK tren\r\n")
        self.assertEqual(qemu_prueba.comprobar_tren(bien), [])
        for antes, despues in [("FLANCOS=4", "FLANCOS=3"), ("NIVEL=0", "NIVEL=1"),
                               ("CANCELADO=1", "CANCELADO=0"),
                               ("TALLER-OK tren", "TALLER-FALLO tren"),
                               ("TREN-INICIAL=1\r\n", "")]:
            with self.subTest(despues=despues):
                self.assertTrue(qemu_prueba.comprobar_tren(bien.replace(antes, despues)))


if __name__ == "__main__":
    unittest.main()
