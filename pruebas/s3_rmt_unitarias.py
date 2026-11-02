"""RMT del S3: verificador puro de ocho y treinta LED, incluso con troceo."""
import contextlib
import io
import socket
import struct
import threading
import unittest
from unittest import mock

import qemu_prueba

V = qemu_prueba.VERSION_PUENTE


def carga(datos, canal=0, gpio=10, tick=100000, final=True):
    """Los datos son bytes GRB; símbolos WS2812 de 1,2 µs y fin explícito."""
    simbolos = [0x00048008 if valor & (1 << bit) else 0x00088004
                for valor in datos for bit in range(7, -1, -1)]
    if final:
        simbolos.append(0)
    return struct.pack("<BBI", canal, gpio, tick) + struct.pack(f"<{len(simbolos)}I", *simbolos)


class PruebaRmtS3(unittest.TestCase):
    def setUp(self):
        self.texto = "TALLER-OK s3_rmt\r\n"
        self.tramas = [(1, V, 0, struct.pack("<I", V)),
                       (7, V, 1000, carga(bytes.fromhex("22 11 33") * 8)),
                       (7, V, 1000000, carga(bytes.fromhex("55 44 66") * 30))]

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_s3_rmt", None)
        self.assertTrue(callable(fn), "falta verificador s3_rmt")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_acepta_colores_longitudes_y_cuatro_canales_tx(self):
        self.assertEqual(self.comprobar(), [])
        for canal in (1, 2, 3):
            otras = [(t, v, n, bytes([canal]) + c[1:] if t == 7 else c)
                     for t, v, n, c in self.tramas]
            self.assertEqual(self.comprobar(tramas=otras), [])

    def test_acepta_troceo_sin_perder_leds_y_fin_separado(self):
        trama = self.tramas[2]
        for cortes in ((6 + 32 * 4,), (6 + 48 * 4, 6 + 719 * 4), (6 + 720 * 4,)):
            limites = (6,) + cortes + (len(trama[3]),)
            trozos = [(7, V, trama[2], trama[3][:6] + trama[3][a:b])
                      for a, b in zip(limites, limites[1:])]
            self.assertEqual(self.comprobar(tramas=self.tramas[:2] + trozos), [])

    def test_rechaza_led_perdido_color_rgb_reordenado_y_trenes_extra(self):
        for datos in (bytes.fromhex("55 44 66") * 29,
                      bytes.fromhex("44 55 66") * 30,
                      bytes.fromhex("55 44 66") * 31):
            self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, V, 1000000, carga(datos))]))
        for tramas in (self.tramas[:2], self.tramas + [self.tramas[-1]],
                       self.tramas[:1] + self.tramas[:0:-1]):
            self.assertTrue(self.comprobar(tramas=tramas))

    def test_rechaza_simbolo_final_ausente_intermedio_o_invalido(self):
        original = self.tramas[2][3]
        for datos in (original[:-4], original[:-4] + struct.pack("<I", 0x8000),
                      original[:10] + bytes(4) + original[14:],
                      original + bytes(4),
                      original[:6] + struct.pack("<I", 0x00078005) + original[10:],
                      original[:6] + struct.pack("<I", 0x80048008) + original[10:]):
            self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, V, 1000000, datos)]))

    def test_rechaza_uart_falsa_incompleta_y_duplicada(self):
        for texto in ("prefijo" + self.texto, self.texto.rstrip(), self.texto * 2,
                      self.texto + "TALLER-OK s3_rmtbasura\n",
                      self.texto + "TALLER-FALLO s3_rmt\n"):
            self.assertTrue(self.comprobar(texto=texto))

    def test_acepta_sello_de_inicio_anterior_al_latido_emitido_durante_tx(self):
        tramas = [self.tramas[0], (8, V, 2000, b""), self.tramas[1],
                  (8, V, 1100000, b""), self.tramas[2]]
        self.assertEqual(self.comprobar(tramas=tramas), [])

    def test_rechaza_canal_gpio_tick_version_tipo_y_carga_invalidos(self):
        for datos in (carga(bytes.fromhex("55 44 66") * 30, canal=4),
                      carga(bytes.fromhex("55 44 66") * 30, gpio=13),
                      carga(bytes.fromhex("55 44 66") * 30, tick=0),
                      carga(bytes.fromhex("55 44 66") * 30, tick=12500),
                      b"", b"1234567"):
            self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, V, 1000000, datos)]))
        for tramas in ([], [(1, 6, 0, struct.pack("<I", 6))] + self.tramas[1:],
                       self.tramas + [(1, V, 2000000, struct.pack("<I", V))],
                       self.tramas[:2] + [(7, 6, 1000000, self.tramas[2][3])],
                       self.tramas + [(3, V, 2000000, b"\x00\x01")],
                       self.tramas + [(8, V, 2000000, b"x")],
                       self.tramas + [(2, V, 2000000, b"\x00")],
                       self.tramas + [(2, V, 2000000, struct.pack("<QQ", 0, 1 << 49))]):
            self.assertTrue(self.comprobar(tramas=tramas))

    def test_rechaza_trozos_con_sello_o_canal_distintos_y_solapamiento(self):
        original = self.tramas[2]
        a, b = original[3][:6 + 48 * 4], original[3][:6] + original[3][6 + 48 * 4:]
        for segunda in ((7, V, 1000001, b), (7, V, 1000000, b"\x01" + b[1:])):
            self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, V, 1000000, a), segunda]))
        for inicio in (999, 200000):
            self.assertTrue(self.comprobar(tramas=self.tramas[:2] + [(7, V, inicio, original[3])]))
        self.assertTrue(self.comprobar(tramas=self.tramas + [(8, V, 2000000, b""),
                                                           (8, V, 1999999, b"")]))

    def test_mascara_gpio_alta_y_hueco_fisico(self):
        alta = (2, V, 2000000, struct.pack("<QQ", 1 << 48, 1 << 48))
        self.assertEqual(self.comprobar(tramas=self.tramas + [alta]), [])
        for pin in (22, 23, 24, 25, 49, 63):
            fuera = (2, V, 2000000, struct.pack("<QQ", 1 << pin, 0))
            self.assertTrue(self.comprobar(tramas=self.tramas + [fuera]))


class PruebaModoRmtS3(unittest.TestCase):
    def test_recoge_troceo_del_puente_y_uart_por_sockets(self):
        puente, externo = socket.socketpair()
        uart, firmware_uart = socket.socketpair()
        errores = []
        caso = PruebaRmtS3()
        caso.setUp()
        proceso = mock.Mock(stdout=None)

        def servidor():
            try:
                tramas = caso.tramas[:2]
                tipo, version, t_ns, carga_rmt = caso.tramas[2]
                tramas += [(tipo, version, t_ns, carga_rmt[:6 + 48 * 4]),
                           (tipo, version, t_ns, carga_rmt[:6] + carga_rmt[6 + 48 * 4:])]
                for tipo, version, t_ns, datos in tramas:
                    externo.sendall(struct.pack("<BBHQ", tipo, version, len(datos), t_ns) + datos)
                firmware_uart.sendall(caso.texto.encode())
            except BaseException as exc:
                errores.append(exc)
            finally:
                externo.close()
                firmware_uart.close()

        hilo = threading.Thread(target=servidor, daemon=True)
        hilo.start()
        try:
            with mock.patch.object(qemu_prueba, "lanzar_s3", return_value=(
                    mock.Mock(), proceso, 1, 2)), \
                 mock.patch.object(qemu_prueba, "conectar", side_effect=lambda p, _: puente if p == 2 else uart), \
                 contextlib.redirect_stdout(io.StringIO()) as salida:
                self.assertEqual(qemu_prueba.s3_rmt("qemu-s3", "imagen.bin"), 0)
            self.assertIn("OK s3_rmt:", salida.getvalue())
        finally:
            puente.close()
            uart.close()
            hilo.join(timeout=4)
        self.assertFalse(hilo.is_alive())
        self.assertEqual(errores, [])


if __name__ == "__main__":
    unittest.main()
