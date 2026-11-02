"""SPI2 C3: MOSI exacto, bus 2, CPU/GDMA y verificación independiente de QEMU."""
import struct
import unittest
import contextlib
import io
import shutil
import socket
import tempfile
import threading
from pathlib import Path
from unittest import mock

import qemu_prueba
from c3_modelos import compilar_ejecutar, ejecutar_modelo

V = qemu_prueba.VERSION_PUENTE


class PruebaSpiC3(unittest.TestCase):
    def setUp(self):
        self.texto = "C3-SPI-MISO=ffffffff\r\nTALLER-OK c3_spi\r\n"
        self.datos = b"abcd" + bytes(range(256)) * 160 + b"\0" * 4
        self.tramas = [(1, V, 0, struct.pack("<I", V))]
        # Se permiten cortes distintos de trama; el contrato exige todos los bytes.
        for i in range(0, len(self.datos), 4093):
            self.tramas.append((3, V, i + 1, b"\x02" + self.datos[i:i + 4093]))

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_c3_spi", None)
        self.assertTrue(callable(fn), "falta verificador c3_spi")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_acepta_todos_los_bytes_y_fragmentacion(self):
        self.assertEqual(self.comprobar(), [])
        self.assertEqual(self.comprobar(self.tramas[:1] + [(3, V, 1, b"\x02" + self.datos)]), [])

    def test_rechaza_byte_perdido_repetido_cambiado_y_desorden(self):
        for datos in (self.datos[:-1], self.datos + b"\0", b"x" + self.datos[1:],
                      self.datos[:1234] + self.datos[1235:],
                      self.datos[4:] + self.datos[:4]):
            with self.subTest(largo=len(datos)):
                self.assertTrue(self.comprobar(self.tramas[:1] + [(3, V, 1, b"\x02" + datos)]))

    def test_rechaza_tiempos_decrecientes_con_datos_correctos(self):
        tramas = list(self.tramas)
        tipo, version, _, carga = tramas[-1]
        tramas[-1] = (tipo, version, 0, carga)
        self.assertTrue(self.comprobar(tramas))

    def test_rechaza_otro_bus_vacio_version_y_sobre_invalido(self):
        for trama in ((3, V, 1, b"\x03" + self.datos), (3, V, 1, b"\x02"),
                      (3, V, 1, b""), (3, 6, 1, b"\x02" + self.datos),
                      (3, V, 1, b"\x02" + bytes(65535)),
                      (18, V, 1, b""), (8, V, 1, b"x")):
            self.assertTrue(self.comprobar(self.tramas + [trama]))
        self.assertTrue(self.comprobar([]))
        self.assertTrue(self.comprobar(self.tramas[1:]))
        self.assertTrue(self.comprobar(self.tramas + [self.tramas[0]]))
        self.assertTrue(self.comprobar(self.tramas + [(8, V, 1, b"")]))

    def test_rechaza_uart_incompleta_duplicada_y_fallo(self):
        for texto in ("", self.texto.rstrip(), self.texto * 2,
                      self.texto.replace("ffffffff", "00000000"),
                      "prefijo" + self.texto, self.texto + "TALLER-FALLO c3_spi\n",
                      "TALLER-OK c3_spi\nC3-SPI-MISO=ffffffff\n"):
            self.assertTrue(self.comprobar(texto=texto))


class PruebaModeloSpiC3(unittest.TestCase):
    def test_modelo_real_cpu_gdma_canales_irq_y_descriptores(self):
        ejecutar_modelo(self, "spi2", "ssi")

    def test_emisor_real_del_puente_bus_2_sin_perdidas(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or shutil.which("cl")
        if not cc:
            self.skipTest("compilador C no disponible")
        raiz = Path(__file__).resolve().parents[1]
        fuente = (raiz / "capas/hw/misc/taller_puente.c").read_text(encoding="utf-8")
        emisor = fuente[fuente.index("void taller_puente_spi_fin("):fuente.index("void taller_puente_gpio(")]
        plantilla = (raiz / "pruebas/spi_dma_puente.c").read_text(encoding="utf-8")
        plantilla = plantilla.replace("carga[0] == 3", "carga[0] == 2").replace("spi_byte(3,", "spi_byte(2,").replace("spi_fin(3)", "spi_fin(2)")
        compilar_ejecutar(plantilla.replace("/* EMISOR_REAL */", emisor), cc)


class PruebaModoSpiC3(unittest.TestCase):
    def test_recoge_spi_por_socket_y_rechaza_datos_corruptos(self):
        for valido in (True, False):
            with self.subTest(valido=valido):
                caso = PruebaSpiC3()
                caso.setUp()
                tramas = caso.tramas if valido else caso.tramas[:-1]
                puente, externo = socket.socketpair()
                uart, firmware_uart = socket.socketpair()
                proceso = mock.Mock(stdout=None)
                errores = []

                def servidor():
                    try:
                        for tipo, version, t_ns, carga in tramas:
                            externo.sendall(struct.pack("<BBHQ", tipo, version, len(carga), t_ns) + carga)
                        firmware_uart.sendall(caso.texto.encode())
                    except BaseException as exc:
                        errores.append(exc)
                    finally:
                        externo.close()
                        firmware_uart.close()

                with tempfile.TemporaryDirectory() as ruta:
                    temporal = mock.Mock(name=ruta)
                    hilo = threading.Thread(target=servidor, daemon=True)
                    hilo.start()
                    try:
                        with mock.patch.object(qemu_prueba, "lanzar_c3", return_value=(
                                temporal, proceso, 1, 2)) as lanzar, \
                             mock.patch.object(qemu_prueba, "conectar", side_effect=lambda p, _: puente if p == 2 else uart), \
                             contextlib.redirect_stdout(io.StringIO()) as salida:
                            self.assertEqual(qemu_prueba.c3_spi("qemu-c3", "imagen.bin"), 0 if valido else 1)
                        lanzar.assert_called_once_with("qemu-c3", "imagen.bin")
                        self.assertIn("OK c3_spi:" if valido else "FALLO c3_spi", salida.getvalue())
                        proceso.kill.assert_called_once()
                        temporal.cleanup.assert_called_once()
                    finally:
                        puente.close()
                        uart.close()
                        hilo.join(timeout=4)
                    self.assertFalse(hilo.is_alive())
                    self.assertEqual(errores, [])


if __name__ == "__main__":
    unittest.main()
