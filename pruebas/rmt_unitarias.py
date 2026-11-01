"""RMT: lógica C compartida con QEMU y verificador de colores en el cable."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import qemu_prueba

V = qemu_prueba.VERSION_PUENTE  # versión del puente que se prueba


COLORES8 = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
            (0, 0, 0), (1, 2, 3), (128, 64, 32), (255, 255, 0)]
COLORES30 = [(i * 7, 255 - i * 5, i * 3) for i in range(30)]


def carga(colores, tick=100000, gpio=13):
    simbolos = []
    for r, g, b in colores:
        for valor in (g, r, b):
            for bit in range(7, -1, -1):
                alto = 8 if valor & (1 << bit) else 4
                simbolos.append(alto | 0x8000 | ((12 - alto) << 16))
    simbolos.append(0)
    return struct.pack("<BBI", 0, gpio, tick) + struct.pack(f"<{len(simbolos)}I", *simbolos)


class PruebaRmt(unittest.TestCase):
    def setUp(self):
        self.tramas = [(1, V, 0, struct.pack("<I", V)),
                       (7, V, 1000, carga(COLORES8)),
                       (7, V, 900000, carga(COLORES30))]
        self.texto = "T_SHOW=240\r\nTALLER-OK rmt\r\n"

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_rmt", None)
        self.assertTrue(callable(fn), "falta verificador RMT")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_colores_tiempos_y_trozos(self):
        self.assertEqual(self.comprobar(), [])
        for t in (200, 2000):
            self.assertEqual(self.comprobar(texto=self.texto.replace("240", str(t))), [])
        trama = self.tramas[2]
        for limite in (6 + 32 * 4, 6 + 719 * 4):
            trozos = [(7, V, trama[2], trama[3][:limite]),
                      (7, V, trama[2], trama[3][:6] + trama[3][limite:])]
            self.assertEqual(self.comprobar(tramas=self.tramas[:2] + trozos), [])

    def test_rechaza_colores_cortos_gpio_version_y_cargas_invalidas(self):
        for c in (carga(COLORES30[:-1]), carga(COLORES30[::-1]),
                  carga(COLORES30, gpio=12), carga(COLORES30, tick=0), b"", b"1234567"):
            with self.subTest(carga=len(c)):
                self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, V, 900000, c)]))
        self.assertTrue(self.comprobar(tramas=self.tramas[:2]))
        self.assertTrue(self.comprobar(tramas=self.tramas + [self.tramas[2]]))
        self.assertTrue(self.comprobar(tramas=[(1, 5, 0, struct.pack("<I", 5))] + self.tramas[1:]))
        self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, 5, 900000, carga(COLORES30))]))

    def test_rechaza_t_show_y_uart_incompletos(self):
        for texto in ("T_SHOW=199\nTALLER-OK rmt\n", "T_SHOW=2001\nTALLER-OK rmt\n",
                      "T_SHOW=240basura\nTALLER-OK rmt\n", "TALLER-OK rmt\n",
                      self.texto + "T_SHOW=240\n", self.texto.replace("OK", "FALLO")):
            self.assertTrue(self.comprobar(texto=texto))

    def test_decodifica_ticks_apb_y_umbral_de_055_us(self):
        fn = getattr(qemu_prueba, "decodificar_rmt", None)
        self.assertTrue(callable(fn))
        datos = carga(COLORES8)
        simbolos = struct.unpack_from(f"<{(len(datos) - 6) // 4}I", datos, 6)
        escalados = [((s & 0x7fff) * 8) | (s & 0x80008000) |
                     ((((s >> 16) & 0x7fff) * 8) << 16) for s in simbolos]
        self.assertEqual(fn(escalados, 12500), COLORES8)
        # 550 ns sigue siendo cero; 560 ns es uno. 24 bits completos GRB.
        for alto, valor in ((55, 0), (56, 255)):
            self.assertEqual(fn([alto | 0x8000 | (100 << 16)] * 24 + [0], 10000),
                             [(valor, valor, valor)])
        with self.assertRaises(ValueError):
            fn([4 | 0x8000 | (8 << 16)] * 23 + [0], 100000)

    def test_rechaza_tiempos_rmt_decrecientes_o_solapados(self):
        for inicio in (999, 200000):
            with self.subTest(inicio=inicio):
                self.assertTrue(self.comprobar(tramas=self.tramas[:2] +
                                              [(7, V, inicio, carga(COLORES30))]))

    def test_modos_previos_aceptan_hola6(self):
        hola = self.tramas[0]
        self.assertTrue(qemu_prueba.es_hola(hola, 5))
        self.assertEqual(qemu_prueba.comprobar_pwm(
            [hola, (4, V, 0, struct.pack("<BII", 13, 50000, 50000))], "TALLER-OK pwm"), [])
        self.assertEqual(qemu_prueba.comprobar_i2c(
            [hola, (5, V, 0, b"\x00\x3c\x00\xaf")],
            "ENCONTRADO 0x3C\nLEIDO=60,61\nTALLER-OK i2c\n"), [])
        self.assertEqual(qemu_prueba.comprobar_armado(
            [hola, (6, V, 1, b"\x04"), (6, V, 2, b"\x04"), (6, V, 3, b"\x05")],
            "DHT=80,80,50,70\nDHT2=80,80,50,70\nCORTO=SIN_RESPUESTA\n"
            "ECO=1166\nECO_CORTO=0\nTALLER-OK armado\n"), [])

    def test_logica_c_real(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar en WSL")
        raiz = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "rmt"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(raiz / "capas/include"),
                            str(raiz / "pruebas/rmt_logica.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_emisor_real_trocea_sin_perder_simbolos(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar en WSL")
        raiz = Path(__file__).resolve().parents[1]
        fuente = (raiz / "capas/hw/misc/taller_puente.c").read_text(encoding="utf-8")
        self.assertIn("void taller_puente_rmt(", fuente)
        cabecera = fuente[fuente.index("static void enviar_en("):fuente.index("static void enviar(")]
        emisor = fuente[fuente.index("void taller_puente_rmt("):fuente.index("void taller_puente_iniciar(")]
        plantilla = (raiz / "pruebas/rmt_emisor.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "emisor.c"
            exe = Path(tmp) / "emisor"
            c.write_text(plantilla.replace("/* EMISOR_REAL */", cabecera + emisor), encoding="utf-8")
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
