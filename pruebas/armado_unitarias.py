"""Lógica C real y verificador del modo armado; no necesitan QEMU."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import qemu_prueba


class PruebaArmado(unittest.TestCase):
    def setUp(self):
        self.texto = ("DHT=80,80,50,70\r\nDHT2=80,80,50,70\r\n"
                      "CORTO=SIN_RESPUESTA\r\nECO=1166\r\nECO_CORTO=0\r\n"
                      "TALLER-OK armado\r\n")
        self.tramas = [(1, 5, 0, struct.pack("<I", 5)),
                       (6, 5, 100, b"\x04"), (6, 5, 200, b"\x04"),
                       (6, 5, 300, b"\x05")]

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_armado", None)
        self.assertTrue(callable(fn), "falta el verificador armado")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_acepta_medidas_y_limites_inclusivos(self):
        self.assertEqual(self.comprobar(), [])
        for valores, eco in [("77,83,47,73", 1163), ("83,77,53,67", 1169)]:
            self.assertEqual(self.comprobar(texto=self.texto.replace(
                "80,80,50,70", valores).replace("1166", str(eco))), [])

    def test_rechaza_medidas_incorrectas_incompletas_o_duplicadas(self):
        for antes, despues in [("80,80,50,70", "76,80,50,70"),
                               ("1166", "1170"), ("1166", "1162"),
                               ("ECO_CORTO=0", "ECO_CORTO=1"),
                               ("CORTO=SIN_RESPUESTA", "CORTO=RESPUESTA"),
                               ("DHT2=", "PREFIJODHT2="),
                               ("TALLER-OK armado", "TALLER-FALLO armado"),
                               ("ECO=1166", "ECO=1166basura"),
                               ("DHT=80,80,50,70", "DHT=80,80,50,70\nDHT=80,80,50,70")]:
            with self.subTest(despues=despues):
                self.assertTrue(self.comprobar(texto=self.texto.replace(antes, despues)))

    def test_exige_exactamente_tres_disparos_validos_y_hola5(self):
        for tramas in [[], self.tramas[1:], self.tramas[:-1],
                       self.tramas + [(6, 5, 400, b"\x05")],
                       self.tramas + [(6, 5, 400, b"\x06")],
                       self.tramas + [(6, 5, 400, b"")],
                       self.tramas + [(6, 4, 400, b"\x04")],
                       [self.tramas[0], self.tramas[2], self.tramas[1], self.tramas[3]],
                       [(1, 4, 0, struct.pack("<I", 4))] + self.tramas[1:]]:
            with self.subTest(tramas=tramas):
                self.assertTrue(self.comprobar(tramas=tramas))

    def test_constructor_sin_relleno_y_limites(self):
        fn = getattr(qemu_prueba, "trama_entrada_armada", None)
        self.assertTrue(callable(fn), "falta ENTRADA_ARMADA")
        self.assertEqual(fn(4, 1, 800000, 4, [(30000, 0), (80000, 1)]),
                         struct.pack("<BBHQ", 20, 5, 19, 0) +
                         bytes.fromhex("04 01 00 35 0c 00 04 02 00 30 75 00 00 00 80 38 01 00 01"))
        self.assertEqual(len(fn(39, 0, 0xffffffff, 39, [(0xffffffff, 1)] * 200)), 1021)
        self.assertEqual(len(fn(4, 1, 0, 4, [])), 21)
        for args in [(40, 1, 0, 4, []), (4, 2, 0, 4, []),
                     (4, 1, -1, 4, []), (4, 1, 0, 40, []),
                     (4, 1, 0, 4, [(1, 2)]), (4, 1, 0, 4, [(-1, 0)]),
                     (4, 1, 0, 4, [(1, 0)] * 201)]:
            with self.subTest(args=args[:4]):
                with self.assertRaises(ValueError):
                    fn(*args)

    def test_modos_previos_aceptan_hola5(self):
        hola = self.tramas[0]
        self.assertEqual(qemu_prueba.comprobar_pwm(
            [hola, (4, 5, 100, struct.pack("<BII", 13, 50000, 50000))], "TALLER-OK pwm"), [])
        self.assertEqual(qemu_prueba.comprobar_i2c(
            [hola, (5, 5, 100, b"\x00\x3c\x00\xaf")],
            "ENCONTRADO 0x3C\nLEIDO=60,61\nTALLER-OK i2c\n"), [])

    def test_logica_c_real(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar esta prueba en WSL")
        raiz = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "armado"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(raiz / "capas/include"),
                            str(raiz / "pruebas/armado_logica.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_receptor_descarta_trama_excesiva_entera_y_conserva_siguiente(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar esta prueba en WSL")
        raiz = Path(__file__).resolve().parents[1]
        fuente = (raiz / "capas/hw/misc/taller_puente.c").read_text(encoding="utf-8")
        # Compila el receptor REAL aislado de QOM/chardev. Solo las llamadas a
        # otros periféricos se sustituyen; el armado usa también su lógica real.
        receptor = fuente[fuente.index("/* Tramas que llegan"):fuente.index("/* Por bus:")]
        clasico = (raiz / "capas/hw/xtensa/esp32.c").read_text(encoding="utf-8")
        receptor_clasico = clasico[clasico.index("static void esp32_taller_recibir"):
                                   clasico.index("static void esp32_soc_realize")]
        c3 = (raiz / "capas/hw/riscv/esp32c3.c").read_text(encoding="utf-8")
        receptor_c3 = c3[c3.index("static void esp32c3_taller_recibir"):
                         c3.index("static void esp32c3_machine_init")]
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "puente_receptor.h").write_text(receptor, encoding="utf-8")
            Path(tmp, "clasico_receptor.h").write_text(receptor_clasico, encoding="utf-8")
            Path(tmp, "c3_receptor.h").write_text(receptor_c3, encoding="utf-8")
            exe = Path(tmp) / "receptor"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-I", str(raiz / "capas/include"),
                            "-I", tmp, str(raiz / "pruebas/armado_receptor.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
