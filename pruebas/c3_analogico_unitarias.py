"""ADC1 C3: verificador puro y registros reales aislados de QOM."""
import struct
import unittest

import qemu_prueba
from c3_modelos import ejecutar_modelo

V = qemu_prueba.VERSION_PUENTE


class PruebaAnalogicoC3(unittest.TestCase):
    def setUp(self):
        self.tramas = [(1, V, 0, struct.pack("<I", V))]
        self.texto = "C3-A=0\r\nC3-A=2048\r\nC3-A=4095\r\nTALLER-OK c3_analogico\r\n"

    def comprobar(self, texto=None, tramas=None):
        fn = getattr(qemu_prueba, "comprobar_c3_analogico", None)
        self.assertTrue(callable(fn), "falta verificador c3_analogico")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_cero_medio_maximo(self):
        self.assertEqual(self.comprobar(), [])

    def test_rechaza_valores_falsos_orden_y_lineas_incompletas(self):
        for texto in (self.texto.replace("2048", "0"), self.texto.replace("4095", "4096"),
                      self.texto.replace("C3-A=0", "C3-A=4095"),
                      self.texto.replace("C3-A=2048", "prefijoC3-A=2048"),
                      self.texto.replace("C3-A=2048", "C3-A=2048basura"),
                      self.texto.replace("TALLER-OK c3_analogico\r\n", "TALLER-OK c3_analogico"),
                      self.texto + "TALLER-FALLO c3_analogico\n", self.texto * 2):
            self.assertTrue(self.comprobar(texto=texto))

    def test_rechaza_hola_ausente_o_antiguo(self):
        for tramas in ([], [(1, 6, 0, struct.pack("<I", 6))], [(1, V, 0, b"")]):
            self.assertTrue(self.comprobar(tramas=tramas))

    def test_modelo_real(self):
        ejecutar_modelo(self, "saradc")


if __name__ == "__main__":
    unittest.main()
