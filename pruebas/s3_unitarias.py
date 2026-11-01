"""S3: mismas garantías C3, más bancos altos, huecos y periféricos propios."""
import struct
import socket
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import c3_gpio_unitarias as gpio_c3
import c3_pwm_unitarias as pwm_c3
import c3_analogico_unitarias as adc_c3
import qemu_prueba
from s3_modelos import ejecutar_modelo


class PruebaGpioS3(gpio_c3.PruebaVerificadorC3):
    def setUp(self):
        super().setUp()
        self.texto = self.texto.replace("C3-", "S3-").replace(" c3", " s3_gpio")
        self.tramas = [(t, v, n, struct.pack("<QQ", struct.unpack("<QQ", c)[0] >> 6,
                                             struct.unpack("<QQ", c)[1] >> 6) if t == 2 else c)
                       for t, v, n, c in self.tramas]

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_s3_gpio", None)
        self.assertTrue(callable(fn), "falta verificador s3_gpio")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_rechaza_uart_incompleta_duplicada_o_falsa(self):
        for texto in (self.texto.replace("ANTES=1", "ANTES=0"), self.texto * 2,
                      self.texto.replace("DESPUES=0", "DESPUES=1"),
                      self.texto + "TALLER-FALLO s3_gpio\n",
                      self.texto.replace("S3-DESPUES=0", "S3-DESPUES=0basura"),
                      self.texto.replace("TALLER-OK s3_gpio\r\n", "TALLER-OK s3_gpio"),
                      "S3-DESPUES=0\nS3-ANTES=1\nTALLER-OK s3_gpio\n"):
            self.assertTrue(self.comprobar(texto=texto))

    def test_banco_alto_valido_y_huecos_rechazados(self):
        self.assertEqual(self.comprobar(tramas=self.tramas + [
            (2, 7, 100, struct.pack("<QQ", 1 << 48, (1 << 48) | 4))]), [])
        for pin in (22, 23, 24, 25, 49, 63):
            self.assertTrue(self.comprobar(tramas=self.tramas + [
                (2, 7, 100, struct.pack("<QQ", 0, (1 << pin) | 4))]))

    def test_modelo_real(self):
        ejecutar_modelo(self, "gpio")


class PruebaPwmS3(pwm_c3.PruebaPwmC3):
    def setUp(self):
        super().setUp()
        self.texto = self.texto.replace("c3_pwm", "s3_pwm")
        self.tramas = [(t, v, n, b"\x04" + c[1:] if t == 4 else c)
                       for t, v, n, c in self.tramas]

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_s3_pwm", None)
        self.assertTrue(callable(fn), "falta verificador s3_pwm")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_modelo_real(self):
        ejecutar_modelo(self, "ledc")

    def test_uart_s3_falsa_y_pines_altos(self):
        for texto in (self.texto * 2, "prefijo" + self.texto,
                      self.texto.rstrip(), self.texto + "TALLER-FALLO s3_pwm\n"):
            self.assertTrue(self.comprobar(texto=texto))
        for pin in (22, 25, 49, 255):
            self.assertTrue(self.comprobar(tramas=self.tramas + [
                (4, 7, 100, struct.pack("<BII", pin, 50000, 50000))]))
        self.assertEqual(self.comprobar(tramas=self.tramas + [
            (4, 7, 100, struct.pack("<BII", 48, 50000, 50000))]), [])


class PruebaAnalogicoS3(adc_c3.PruebaAnalogicoC3):
    def setUp(self):
        super().setUp()
        self.texto = self.texto.replace("C3-", "S3-").replace("c3_analogico", "s3_analogico")

    def comprobar(self, texto=None, tramas=None):
        fn = getattr(qemu_prueba, "comprobar_s3_analogico", None)
        self.assertTrue(callable(fn), "falta verificador s3_analogico")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_rechaza_valores_falsos_orden_y_lineas_incompletas(self):
        for texto in (self.texto.replace("2048", "0"), self.texto.replace("4095", "4096"),
                      self.texto.replace("S3-A=0", "S3-A=4095"),
                      self.texto.replace("S3-A=2048", "S3-A=2048basura"),
                      self.texto.replace("TALLER-OK s3_analogico\r\n", "TALLER-OK s3_analogico"),
                      self.texto + "TALLER-FALLO s3_analogico\n", self.texto * 2):
            self.assertTrue(self.comprobar(texto=texto))

    def test_modelo_real(self):
        ejecutar_modelo(self, "saradc")


class PruebaModosS3(unittest.TestCase):
    def test_traza_adc_sale_por_stderr_sin_llenar_tuberia(self):
        with tempfile.TemporaryDirectory() as tmp:
            flash = Path(tmp) / "s3.bin"
            flash.write_bytes(b"\xff" * (4 * 1024 * 1024))
            for valor in ("1", "0", ""):
                with self.subTest(valor=valor), \
                     mock.patch.dict("os.environ", {"TALLER_ADC_TRAZA": valor}), \
                     mock.patch.object(qemu_prueba.subprocess, "Popen") as abrir:
                    privado, _, _, _ = qemu_prueba.lanzar_s3("qemu", str(flash))
                    try:
                        self.assertEqual(abrir.call_args.kwargs["stdout"], qemu_prueba.subprocess.PIPE)
                        self.assertEqual(abrir.call_args.kwargs["stderr"],
                                         None if valor == "1" else qemu_prueba.subprocess.STDOUT)
                    finally:
                        privado.cleanup()

    def test_flash_incompleta_no_arranca(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(qemu_prueba.subprocess, "Popen") as abrir:
            flash = Path(tmp) / "corta.bin"
            flash.write_bytes(b"\xff" * 100)
            with self.assertRaisesRegex(ValueError, "4 MiB"):
                qemu_prueba.lanzar_s3("qemu", str(flash))
            abrir.assert_not_called()

    def test_tren_en_gpio48_y_limite_del_clasico(self):
        fn = qemu_prueba.trama_entrada_armada
        with self.assertRaises(ValueError):
            fn(48, 1, 100, 47, [(50, 0)])
        self.assertEqual(fn(48, 1, 100, 47, [(50, 0)], pines=49),
                         struct.pack("<BBHQ", 20, 5, 14, 0) +
                         bytes.fromhex("30 01 64 00 00 00 2f 01 00 32 00 00 00 00"))
        with self.assertRaises(ValueError):
            fn(49, 1, 100, 47, [(50, 0)], pines=49)

    def test_modos_por_socket_y_flash_privada(self):
        for modo in ("s3_gpio", "s3_pwm", "s3_analogico"):
            with self.subTest(modo=modo):
                self.ejecutar_modo(modo)

    def ejecutar_modo(self, modo):
        puente, externo = socket.socketpair()
        uart, firmware_uart = socket.socketpair()
        errores, comandos = [], []
        proceso = mock.Mock(stdout=None)
        proceso.wait.return_value = 0

        def recibir(n):
            buf = b""
            while len(buf) < n:
                trozo = externo.recv(n - len(buf))
                if not trozo:
                    raise EOFError("faltó inyección del lanzador S3")
                buf += trozo
            return buf

        def servidor():
            try:
                externo.settimeout(3)
                externo.sendall(qemu_prueba.CABECERA.pack(1, 7, 4, 0) + struct.pack("<I", 7))
                if modo == "s3_gpio":
                    for n, nivel in enumerate((0, 1, 0, 1, 0, 1, 0)):
                        externo.sendall(qemu_prueba.CABECERA.pack(2, 7, 16, n + 1) +
                                        struct.pack("<QQ", nivel << 2, 4))
                    # Separar el salto de línea reproduce la fragmentación real de UART.
                    firmware_uart.sendall(b"S3-ANTES=1\r")
                    threading.Event().wait(0.03)
                    firmware_uart.sendall(b"\n")
                    self.assertEqual(recibir(14), qemu_prueba.CABECERA.pack(16, 6, 2, 0) + b"\x04\x00")
                    firmware_uart.sendall(b"S3-DESPUES=0\r\nTALLER-OK s3_gpio\r\n")
                elif modo == "s3_pwm":
                    for n, (f, c) in enumerate(((50000, 49987), (50000, 250000),
                                                 (100000, 250000), (100000, 0))):
                        externo.sendall(qemu_prueba.CABECERA.pack(4, 7, 9, n + 1) + struct.pack("<BII", 4, f, c))
                    firmware_uart.sendall(b"TALLER-OK s3_pwm\r\n")
                else:
                    firmware_uart.sendall(b"S3-A=0\r\n")
                    self.assertEqual(recibir(15), qemu_prueba.trama_analogico(1, 2048))
                    firmware_uart.sendall(b"S3-A=2048\r\n")
                    self.assertEqual(recibir(15), qemu_prueba.trama_analogico(1, 4095))
                    firmware_uart.sendall(b"S3-A=4095\r\nTALLER-OK s3_analogico\r\n")
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
                with imagen.open("r+b") as archivo:
                    archivo.write(b"QEMU")
                return proceso

            hilo = threading.Thread(target=servidor, daemon=True)
            hilo.start()
            try:
                with mock.patch.object(qemu_prueba.subprocess, "Popen", side_effect=arrancar), \
                     mock.patch.object(qemu_prueba, "puerto_libre", side_effect=[1, 2]), \
                     mock.patch.object(qemu_prueba, "conectar", side_effect=lambda p, _: puente if p == 2 else uart):
                    self.assertEqual(getattr(qemu_prueba, modo)("qemu-system-xtensa", str(original)), 0)
                self.assertEqual(original.read_bytes(), contenido)
                cmd = comandos[0]
                self.assertEqual(cmd[cmd.index("-machine") + 1], "esp32s3")
                self.assertFalse(any("efuse" in arg for arg in cmd))
                self.assertIn("driver=timer.esp32s3.timg,property=wdt_disable,value=true", cmd)
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
