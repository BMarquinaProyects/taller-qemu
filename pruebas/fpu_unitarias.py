"""Regresión M-160: salida UART exacta y modos FPU sin ejecutar QEMU."""
import contextlib
import io
import unittest
from unittest import mock

import qemu_prueba

BUENO = "TALLER fpu cpenable=0xff\nTALLER-OK fpu 105 -21 3\n"


class PruebaVerificadorFpu(unittest.TestCase):
    def test_lineas_exactas_con_lf_y_crlf(self):
        for texto in (BUENO, BUENO.replace("\n", "\r\n"), "Arranque\n" + BUENO):
            with self.subTest(texto=texto):
                self.assertEqual(qemu_prueba.comprobar_fpu(texto), [])

    def test_cpenable_cero(self):
        self.assertTrue(qemu_prueba.comprobar_fpu(BUENO.replace("0xff", "0x0")))

    def test_resultados_incorrectos(self):
        for resultado in ("104 -21 3", "105 21 3", "105 -21 4"):
            with self.subTest(resultado=resultado):
                self.assertTrue(qemu_prueba.comprobar_fpu(BUENO.replace("105 -21 3", resultado)))

    def test_lineas_ausentes_duplicadas_o_invertidas(self):
        primera, segunda = BUENO.splitlines(keepends=True)
        for texto in ("", primera, segunda, primera + BUENO, BUENO + segunda, segunda + primera):
            with self.subTest(texto=texto):
                self.assertTrue(qemu_prueba.comprobar_fpu(texto))

    def test_no_acepta_prefijos_sufijos_ni_linea_sin_terminar(self):
        for texto in ("prefijo " + BUENO, BUENO.replace("0xff", "0xff extra"),
                      BUENO.replace("105 -21 3", "105 -21 30"), BUENO.rstrip("\n")):
            with self.subTest(texto=texto):
                self.assertTrue(qemu_prueba.comprobar_fpu(texto))

    def test_fallo_del_firmware_incluso_con_resultados_correctos(self):
        for fallo in ("TALLER-FALLO", "Guru Meditation"):
            with self.subTest(fallo=fallo):
                self.assertTrue(qemu_prueba.comprobar_fpu(fallo + "\n" + BUENO))


class PruebaModosFpu(unittest.TestCase):
    def ejecutar(self, modo, trozos, *, reloj=None):
        temporal = mock.Mock()
        proceso = mock.Mock(stdout=io.BytesIO())
        uart = mock.Mock()
        uart.recv.side_effect = trozos
        puente = mock.Mock()
        salida = io.StringIO()
        conexiones = []

        def conectar(puerto, proc):
            self.assertIs(proc, proceso)
            conexiones.append(puerto)
            return puente if puerto == 2 else uart

        with mock.patch.object(qemu_prueba, "lanzar", return_value=(temporal, proceso, 1, None)) as clasico, \
             mock.patch.object(qemu_prueba, "lanzar_s3", return_value=(temporal, proceso, 1, 2)) as s3, \
             mock.patch.object(qemu_prueba, "conectar", side_effect=conectar), \
             contextlib.redirect_stdout(salida):
            if reloj is None:
                resultado = getattr(qemu_prueba, modo)("qemu", "firmware.bin")
            else:
                with mock.patch.object(qemu_prueba.time, "monotonic", side_effect=reloj):
                    resultado = getattr(qemu_prueba, modo)("qemu", "firmware.bin")
            if modo == "fpu":
                clasico.assert_called_once_with("qemu", "firmware.bin", con_puente=False)
                s3.assert_not_called()
                self.assertEqual(conexiones, [1])
            else:
                s3.assert_called_once_with("qemu", "firmware.bin")
                clasico.assert_not_called()
                self.assertEqual(conexiones, [2, 1])
                puente.close.assert_called_once()
        proceso.kill.assert_called_once()
        proceso.wait.assert_called_once()
        self.assertTrue(proceso.stdout.closed)
        uart.close.assert_called_once()
        temporal.cleanup.assert_called_once()
        return resultado, salida.getvalue(), uart

    def test_ambos_modos_completan_marca_y_resultados_fragmentados(self):
        for modo in ("fpu", "s3_fpu"):
            with self.subTest(modo=modo):
                resultado, salida, _ = self.ejecutar(modo, [
                    b"TALLER fpu cpenable=0xff\r\nTALLER-OK fpu",
                    b" 105 -21 3\r", b"\n"])
                self.assertEqual(resultado, 0)
                self.assertIn("OK " + modo, salida)
                self.assertNotIn("FALLO", salida)

    def test_ambos_modos_muestran_uart_y_devuelven_error(self):
        for modo in ("fpu", "s3_fpu"):
            with self.subTest(modo=modo):
                malo = BUENO.replace("0xff", "0x0")
                resultado, salida, _ = self.ejecutar(modo, [malo.encode()])
                self.assertNotEqual(resultado, 0)
                self.assertIn("FALLO " + modo, salida)
                self.assertIn(malo, salida)

    def test_uart_cerrada_sin_resultado_falla(self):
        resultado, salida, _ = self.ejecutar("fpu", [b"Arranque sin FPU\n", b""])
        self.assertNotEqual(resultado, 0)
        self.assertIn("FALLO fpu", salida)
        self.assertIn("Arranque sin FPU", salida)

    def test_fragmento_final_no_recibe_otro_plazo_de_30_segundos(self):
        # La marca llega a los 29 s; el resto ya cae fuera del plazo total.
        resultado, salida, uart = self.ejecutar("s3_fpu", [
            b"TALLER fpu cpenable=0xff\nTALLER-OK fpu"],
            reloj=[0, 29, 30])
        self.assertNotEqual(resultado, 0)
        self.assertIn("FALLO s3_fpu", salida)
        self.assertEqual(uart.recv.call_count, 1)


if __name__ == "__main__":
    unittest.main()
