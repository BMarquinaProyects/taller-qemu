"""UNHILO (v9): lógica C real de 1-Wire, constructor de la trama y verificador; no necesitan QEMU."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import qemu_prueba


class PruebaUnHilo(unittest.TestCase):
    def test_logica_c(self):
        cc = (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or
              shutil.which("cl"))
        if cc is None:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        fuente = raiz / "pruebas" / "unhilo_logica.c"
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "unhilo.exe"
            if Path(cc).stem.lower() == "cl":
                comando = [cc, "/nologo", "/std:c11", "/W4", "/WX", "/wd4100",
                           "/wd4244", "/wd4267", "/utf-8", f"/I{raiz / 'capas/include'}",
                           str(fuente), f"/Fe:{exe}", f"/Fo:{Path(tmp) / 'unhilo.obj'}"]
            else:
                comando = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           "-I", str(raiz / "capas/include"), str(fuente), "-o", str(exe)]
            subprocess.run(comando, check=True)
            subprocess.run([str(exe)], check=True)

    def test_trama_y_rom(self):
        rom = qemu_prueba.rom_ds18b20(bytes([0x11, 0x4B, 0x46, 0x7F, 0x0C, 0x10]))
        self.assertEqual(rom[0], 0x28)
        self.assertEqual(qemu_prueba.crc8_unhilo(rom[:7]), rom[7])
        self.assertEqual(qemu_prueba.crc8_unhilo(bytes([0x02, 0x1C, 0xB8, 0x01, 0, 0, 0])), 0xA2)
        t = qemu_prueba.trama_unhilo(4, rom, -162)
        self.assertEqual(t[:4], struct.pack("<BBH", 22, 9, 12))
        self.assertEqual(t[12:], bytes([4]) + rom + struct.pack("<hB", -162, 1))
        with self.assertRaises(ValueError):
            qemu_prueba.trama_unhilo(4, rom[:7], 0)

    def test_verificador(self):
        sensores = {"AA": (21.5, 21.5)}
        bueno = "N=1\r\nPARASITO=0\r\nT12 ROM=AA T=21.5000\r\nT9 ROM=AA T=21.5000\r\nTALLER-OK unhilo\r\n"
        self.assertEqual(qemu_prueba.comprobar_unhilo(bueno, sensores), [])
        self.assertTrue(qemu_prueba.comprobar_unhilo(bueno.replace("T=21.5000\r\nTALLER", "T=85.0000\r\nTALLER"), sensores))


if __name__ == "__main__":
    unittest.main()
