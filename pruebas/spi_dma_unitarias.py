"""DMA SPI: contratos C, puente real y comprobación de MicroPython."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
import struct
import socket
import threading

import qemu_prueba


class PruebaSpiDmaC(unittest.TestCase):
    def test_logica_dma(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar en WSL")
        raiz = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "spi_dma"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(raiz / "capas/include"),
                            str(raiz / "pruebas/spi_dma_logica.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_emisor_spi_real(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar en WSL")
        raiz = Path(__file__).resolve().parents[1]
        fuente = (raiz / "capas/hw/misc/taller_puente.c").read_text(encoding="utf-8")
        emisor = fuente[fuente.index("void taller_puente_spi_fin("):
                        fuente.index("void taller_puente_gpio(")]
        plantilla = (raiz / "pruebas/spi_dma_puente.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            c, exe = Path(tmp) / "emisor.c", Path(tmp) / "emisor"
            c.write_text(plantilla.replace("/* EMISOR_REAL */", emisor), encoding="utf-8")
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_modelo_spi_real(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar en WSL")
        raiz = Path(__file__).resolve().parents[1]
        fuente = (raiz / "capas/hw/ssi/esp32_spi.c").read_text(encoding="utf-8")
        cabecera = (raiz / "capas/include/hw/ssi/esp32_spi.h").read_text(encoding="utf-8")
        cabecera = "\n".join(l for l in cabecera.splitlines()
                             if not l.startswith(("#include", "#pragma")))
        modelo = fuente[fuente.index("enum {"):fuente.index("static const MemoryRegionOps")]
        reset = fuente[fuente.index("static void esp32_spi_reset_hold"):fuente.index("static void esp32_spi_realize")]
        plantilla = (raiz / "pruebas/spi_dma_modelo.c").read_text(encoding="utf-8")
        codigo = plantilla.replace("/* CABECERA_REAL */", cabecera).replace(
            "/* MODELO_REAL */", modelo + reset)
        codigo = codigo.replace("Object *obj, ResetType type", "void *obj, int type")
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "modelo.c"
            exe = Path(tmp) / "modelo"
            c.write_text(codigo, encoding="utf-8")
            subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-I", str(raiz / "capas/include"),
                            str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


class PruebaVerificadorSpiDma(unittest.TestCase):
    def setUp(self):
        self.datos = b"abcd" + bytes(range(256)) * 160 + b"\0" * 4
        self.tramas = [(1, 6, 0, struct.pack("<I", 6))]
        self.tramas += [(3, 6, i, b"\x03" + self.datos[i:i + 4092])
                        for i in range(0, len(self.datos), 4092)]
        self.texto = "MISO=ffffffff\r\nTALLER-OK spi_dma\r\n"

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_spi_dma", None)
        self.assertTrue(callable(fn), "falta verificador SPI DMA")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_todos_los_bytes_en_orden_con_troceo_variable(self):
        self.assertEqual(self.comprobar(), [])
        self.assertEqual(self.comprobar(tramas=self.tramas[:1] +
                                       [(3, 6, 1, b"\x03" + self.datos)]), [])

    def test_rechaza_perdidas_duplicados_bus_y_corrupcion(self):
        for datos in (self.datos[:-1], self.datos + b"\0", self.datos[::-1],
                      self.datos[:10] + b"\xff" + self.datos[11:]):
            self.assertTrue(self.comprobar(tramas=self.tramas[:1] + [(3, 6, 1, b"\x03" + datos)]))
        for carga in (b"", b"\x03", b"\x02" + self.datos):
            self.assertTrue(self.comprobar(tramas=self.tramas[:1] + [(3, 6, 1, carga)]))
        self.assertTrue(self.comprobar(tramas=self.tramas + [self.tramas[-1]]))
        self.assertTrue(self.comprobar(tramas=[(1, 5, 0, struct.pack("<I", 5))] + self.tramas[1:]))

    def test_exige_rx_y_final_sin_excepcion(self):
        for texto in ("TALLER-OK spi_dma\n", "MISO=00000000\nTALLER-OK spi_dma\n",
                      self.texto + "Traceback (most recent call last):\n",
                      self.texto.replace("OK", "FALLO"), self.texto * 2):
            self.assertTrue(self.comprobar(texto=texto))


class PruebaRawPaste(unittest.TestCase):
    def ejecutar(self, stderr=b""):
        cliente, dispositivo = socket.socketpair()
        codigo = "print('abcdefghijk')\n" * 20
        recibido = bytearray()
        fallos = []

        def servidor():
            try:
                dispositivo.settimeout(3)

                def leer(n):
                    datos = b""
                    while len(datos) < n:
                        trozo = dispositivo.recv(n - len(datos))
                        if not trozo:
                            raise EOFError("cliente cerrado")
                        datos += trozo
                    return datos

                self.assertEqual(leer(4), b"\r\x03\x03\x01")
                dispositivo.sendall(b"raw REPL; CTRL-B to exit\r\n>")
                self.assertEqual(leer(3), b"\x05A\x01")
                dispositivo.sendall(b"R\x01\x08\x00")
                while len(recibido) < len(codigo):
                    n = min(8, len(codigo) - len(recibido))
                    recibido.extend(leer(n))
                    if n == 8:
                        dispositivo.sendall(b"\x01")
                self.assertEqual(leer(1), b"\x04")
                dispositivo.sendall(b"\x04salida\r\n\x04" + stderr + b"\x04>")
            except BaseException as exc:
                fallos.append(exc)
            finally:
                dispositivo.close()

        hilo = threading.Thread(target=servidor)
        hilo.start()
        try:
            salida = qemu_prueba.ejecutar_raw_paste(cliente, codigo, 3)
            self.assertEqual(salida, "salida\r\n")
        finally:
            cliente.close()
            hilo.join(timeout=4)
            self.assertFalse(hilo.is_alive(), "servidor raw paste bloqueado")
            if fallos:
                raise fallos[0]
            self.assertEqual(recibido, codigo.encode())

    def test_credito_ventana_y_eof(self):
        self.ejecutar()

    def test_excepcion_remota_es_fallo_aunque_haya_stdout(self):
        with self.assertRaisesRegex(RuntimeError, "fallo remoto"):
            self.ejecutar(b"fallo remoto")

    def test_cierre_uart_y_plazo_no_entran_en_bucle(self):
        cliente, dispositivo = socket.socketpair()
        dispositivo.close()
        with cliente:
            with self.assertRaises((EOFError, OSError)):
                qemu_prueba.ejecutar_raw_paste(cliente, "pass", 0.1)
        cliente, dispositivo = socket.socketpair()
        try:
            with self.assertRaises(TimeoutError):
                qemu_prueba.ejecutar_raw_paste(cliente, "pass", 0.03)
        finally:
            cliente.close()
            dispositivo.close()
