"""GPIO C3 real aislado y verificador de integración; ejecutar también en WSL."""
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import qemu_prueba


class PruebaVerificadorC3(unittest.TestCase):
    def setUp(self):
        self.texto = "C3-ANTES=1\r\nC3-DESPUES=0\r\nTALLER-OK c3\r\n"
        self.tramas = [(1, 6, 0, struct.pack("<I", 6))]
        for i, nivel in enumerate((0, 1, 0, 1, 0, 1, 0)):
            self.tramas.append((2, 6, i + 1, struct.pack("<QQ", nivel << 8, 1 << 8)))

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_c3_gpio", None)
        self.assertTrue(callable(fn), "falta verificador c3_gpio")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_tres_pulsos_y_entrada(self):
        self.assertEqual(self.comprobar(), [])
        # Cambios ajenos y repeticiones del mismo nivel no son otro pulso.
        self.assertEqual(self.comprobar(tramas=self.tramas[:3] +
                                       [self.tramas[2]] + self.tramas[3:]), [])

    def test_rechaza_pulsos_perdidos_sobrantes_y_sin_enable(self):
        for tramas in (self.tramas[:-1], self.tramas[:-2],
                       self.tramas + self.tramas[-2:],
                       self.tramas[:1] + [(2, 6, i, struct.pack("<QQ", n << 8, 0))
                                          for i, n in enumerate((0, 1, 0, 1, 0, 1, 0))]):
            self.assertTrue(self.comprobar(tramas=tramas))

    def test_rechaza_protocolo_pin_fuera_de_rango_y_trama_rota(self):
        for trama in ((2, 5, 20, struct.pack("<QQ", 0, 256)),
                      (2, 6, 20, struct.pack("<QQ", 1 << 22, 256)),
                      (2, 6, 20, struct.pack("<QQ", 0, 1 << 22)),
                      (2, 6, 20, b"\0")):
            self.assertTrue(self.comprobar(tramas=self.tramas + [trama]))
        self.assertTrue(self.comprobar(tramas=[(1, 5, 0, struct.pack("<I", 5))] + self.tramas[1:]))
        self.assertTrue(self.comprobar(tramas=self.tramas[:2] + self.tramas[:0:-1]))

    def test_rechaza_uart_incompleta_duplicada_o_falsa(self):
        for texto in (self.texto.replace("ANTES=1", "ANTES=0"),
                      self.texto.replace("DESPUES=0", "DESPUES=1"),
                      self.texto + "TALLER-FALLO c3\n", self.texto * 2,
                      self.texto.replace("TALLER-OK c3", "prefijoTALLER-OK c3"),
                      self.texto.replace("C3-DESPUES=0", "C3-DESPUES=0basura"),
                      "C3-DESPUES=0\nC3-ANTES=1\nTALLER-OK c3\n"):
            self.assertTrue(self.comprobar(texto=texto))


class PruebaModeloC3(unittest.TestCase):
    def test_gpio_real(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("compilador C no disponible; ejecutar en WSL")
        raiz = Path(__file__).resolve().parents[1]
        fuente = (raiz / "capas/hw/gpio/esp32c3_gpio.c").read_text(encoding="utf-8")
        cab = (raiz / "capas/include/hw/gpio/esp32c3_gpio.h").read_text(encoding="utf-8")
        cab = "\n".join(l for l in cab.splitlines() if not l.startswith(("#include", "#pragma")))
        modelo = fuente[fuente.index("void esp32c3_gpio_fijar_entrada"):
                        fuente.index("static const MemoryRegionOps")]
        reset = fuente[fuente.index("static void esp32c3_gpio_reset_hold"):
                       fuente.index("static void esp32c3_gpio_init")]
        plantilla = (raiz / "pruebas/c3_gpio_modelo.c").read_text(encoding="utf-8")
        codigo = plantilla.replace("/* CABECERA_REAL */", cab).replace("/* MODELO_REAL */", modelo + reset)
        with tempfile.TemporaryDirectory() as tmp:
            c, exe = Path(tmp) / "modelo.c", Path(tmp) / "modelo"
            c.write_text(codigo, encoding="utf-8")
            subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-I", str(raiz / "capas/include"),
                            str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


class PruebaModoC3(unittest.TestCase):
    def test_flash_privada_y_entrada_por_socket_tras_lectura_inicial(self):
        puente, externo = socket.socketpair()
        uart, firmware_uart = socket.socketpair()
        errores = []
        comandos = []
        proceso = mock.Mock(stdout=None)
        proceso.wait.return_value = 0
        tramas = PruebaVerificadorC3()
        tramas.setUp()

        def servidor():
            try:
                externo.settimeout(3)
                for tipo, version, t_ns, carga in tramas.tramas:
                    externo.sendall(qemu_prueba.CABECERA.pack(tipo, version, len(carga), t_ns) + carga)
                firmware_uart.sendall(b"C3-ANTES=1\r\n")
                entrada = bytearray()
                while len(entrada) < 14:
                    trozo = externo.recv(14 - len(entrada))
                    if not trozo:
                        raise EOFError("el modo cerró sin inyectar ENTRADA")
                    entrada.extend(trozo)
                self.assertEqual(entrada, qemu_prueba.CABECERA.pack(16, 6, 2, 0) + b"\x09\x00")
                firmware_uart.sendall(b"C3-DESPUES=0\r\nTALLER-OK c3\r\n")
            except BaseException as exc:
                errores.append(exc)
            finally:
                externo.close()
                firmware_uart.close()

        with tempfile.TemporaryDirectory() as tmp:
            original = Path(tmp) / "original.bin"
            contenido = b"\xff" * (4 * 1024 * 1024)
            original.write_bytes(contenido)

            def arrancar(cmd, **kwargs):
                comandos.append(cmd)
                imagen = Path(cmd[cmd.index("-drive") + 1].split(",")[0][5:])
                self.assertNotEqual(imagen, original)
                self.assertEqual(imagen.read_bytes(), contenido)
                # Simular una escritura de la flash por QEMU.
                with imagen.open("r+b") as archivo:
                    archivo.write(b"QEMU")
                return proceso

            hilo = threading.Thread(target=servidor, daemon=True)
            hilo.start()
            try:
                with mock.patch.object(qemu_prueba.subprocess, "Popen", side_effect=arrancar), \
                     mock.patch.object(qemu_prueba, "puerto_libre", side_effect=[1, 2]), \
                     mock.patch.object(qemu_prueba, "conectar", side_effect=lambda p, _: puente if p == 2 else uart):
                    self.assertEqual(qemu_prueba.c3_gpio("qemu-system-riscv32", str(original)), 0)
                self.assertEqual(original.read_bytes(), contenido)
                cmd = comandos[0]
                self.assertEqual(cmd[cmd.index("-machine") + 1], "esp32c3")
                self.assertFalse(any("efuse" in arg for arg in cmd))
                self.assertIn("driver=timer.esp32c3.timg,property=wdt_disable,value=true", cmd)
                imagen = Path(cmd[cmd.index("-drive") + 1].split(",")[0][5:])
                self.assertFalse(imagen.exists())
                proceso.kill.assert_called_once()
                proceso.wait.assert_called_once()
            finally:
                puente.close()
                uart.close()
                hilo.join(timeout=4)
            self.assertFalse(hilo.is_alive())
            self.assertEqual(errores, [])


if __name__ == "__main__":
    unittest.main()
