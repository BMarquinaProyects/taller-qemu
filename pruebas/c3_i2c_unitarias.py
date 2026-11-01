"""I2C del C3: verificador puro del espejo, ACK, NACK y bytes escritos."""
import contextlib
import io
import socket
import struct
import tempfile
import threading
import unittest
from unittest import mock

import qemu_prueba

V = qemu_prueba.VERSION_PUENTE


class PruebaI2cC3(unittest.TestCase):
    def setUp(self):
        self.texto = ("C3-I2C-ID=AB\r\nC3-I2C-ACK=0\r\nC3-I2C-NACK=2\r\n"
                      "TALLER-OK c3_i2c\r\n")
        self.tramas = [(1, V, 0, struct.pack("<I", V)),
                       (5, V, 100, b"\x00\x3c\xd0"),
                       (5, V, 200, b"\x00\x3c\x00\xaf")]

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_c3_i2c", None)
        self.assertTrue(callable(fn), "falta verificador c3_i2c")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_acepta_espejo_ack_nack_y_escritura_exacta(self):
        self.assertEqual(self.comprobar(), [])
        self.assertEqual(self.comprobar(tramas=self.tramas + [(8, V, 300, b"")]), [])

    def test_rechaza_lectura_ack_nack_incorrectos_y_orden_uart(self):
        for texto in (self.texto.replace("ID=AB", "ID=00"),
                      self.texto.replace("ACK=0", "ACK=2"),
                      self.texto.replace("NACK=2", "NACK=0"),
                      self.texto.replace("ID=AB\r\nC3-I2C-ACK=0", "ACK=0\r\nC3-I2C-ID=AB"),
                      self.texto.replace("C3-I2C-ID=AB", "prefijoC3-I2C-ID=AB"),
                      self.texto.replace("C3-I2C-ID=AB", "C3-I2C-ID=ABbasura"),
                      self.texto.rstrip(), self.texto * 2,
                      self.texto + "C3-I2C-ID=00\n",
                      self.texto + "TALLER-FALLO c3_i2c\n"):
            with self.subTest(texto=texto):
                self.assertTrue(self.comprobar(texto=texto))

    def test_rechaza_escritura_ausente_corta_duplicada_o_en_otro_bus(self):
        for carga in (b"", b"\x00\x3c", b"\x00\x3c\x00", b"\x00\x3c\xaf",
                      b"\x01\x3c\x00\xaf", b"\x00\x48\x00\xaf",
                      b"\x00\x80\x00\xaf", b"\x00\x3c\x00\xaf\x00"):
            with self.subTest(carga=carga):
                self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(5, V, 200, carga)]))
        self.assertTrue(self.comprobar(tramas=self.tramas[:2]))
        self.assertTrue(self.comprobar(tramas=self.tramas + [self.tramas[-1]]))

    def test_rechaza_hola_version_tipo_longitud_y_tiempo_invalidos(self):
        for tramas in ([], self.tramas[1:],
                       [(1, 6, 0, struct.pack("<I", 6))] + self.tramas[1:],
                       [(1, V, 0, b"\x07")] + self.tramas[1:],
                       self.tramas + [(1, V, 300, struct.pack("<I", V))],
                       self.tramas + [(5, 6, 300, b"\x00\x3c\x01")],
                       self.tramas + [(18, V, 300, b"\x00\x3c\x01")],
                       self.tramas + [(8, V, 300, b"x")],
                       self.tramas + [(2, V, 300, b"\x00")],
                       self.tramas + [(2, V, 300, struct.pack("<QQ", 1 << 22, 0))],
                       self.tramas + [(8, V, 99, b"")]):
            with self.subTest(tramas=tramas):
                self.assertTrue(self.comprobar(tramas=tramas))


class PruebaModoI2cC3(unittest.TestCase):
    def test_configura_dispositivo_y_registro_por_socket_antes_de_leer(self):
        puente, externo = socket.socketpair()
        uart, firmware_uart = socket.socketpair()
        errores = []
        caso = PruebaI2cC3()
        caso.setUp()
        proceso = mock.Mock(stdout=None)
        proceso.poll.return_value = None

        def servidor():
            try:
                externo.settimeout(3)
                externo.sendall(struct.pack("<BBHQI", 1, V, 4, 0, V))
                recibidos = bytearray()
                while len(recibidos) < 31:
                    trozo = externo.recv(31 - len(recibidos))
                    if not trozo:
                        raise EOFError("el modo cerró sin configurar el espejo I2C")
                    recibidos.extend(trozo)
                self.assertEqual(recibidos,
                                 struct.pack("<BBHQ", 18, 4, 3, 0) + b"\x00\x3c\x01" +
                                 struct.pack("<BBHQ", 19, 4, 4, 0) + b"\x00\x3c\xd0\xab")
                for tipo, version, t_ns, carga in caso.tramas[1:]:
                    externo.sendall(struct.pack("<BBHQ", tipo, version, len(carga), t_ns) + carga)
                firmware_uart.sendall(caso.texto.encode())
            except BaseException as exc:
                errores.append(exc)
            finally:
                externo.close()
                firmware_uart.close()

        temporal = tempfile.TemporaryDirectory()
        with temporal:
            hilo = threading.Thread(target=servidor, daemon=True)
            hilo.start()
            try:
                with mock.patch.object(qemu_prueba, "lanzar_c3", return_value=(
                        temporal, proceso, 1, 2)), \
                     mock.patch.object(qemu_prueba, "conectar", side_effect=lambda p, _: puente if p == 2 else uart), \
                     contextlib.redirect_stdout(io.StringIO()) as salida:
                    self.assertEqual(qemu_prueba.c3_i2c("qemu-c3", "imagen.bin"), 0)
                self.assertIn("OK c3_i2c:", salida.getvalue())
            finally:
                puente.close()
                uart.close()
                hilo.join(timeout=4)
            self.assertFalse(hilo.is_alive())
            self.assertEqual(errores, [])


if __name__ == "__main__":
    unittest.main()
