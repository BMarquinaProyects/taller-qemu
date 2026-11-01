"""Ejecuta el RMT TX C3 real con reloj virtual y transporte aislados."""
import unittest

from c3_modelos import ejecutar_modelo


class PruebaModeloRmtC3(unittest.TestCase):
    def test_transmision_real_corta_larga_irq_relojes_y_reset(self):
        ejecutar_modelo(self, "rmt")


if __name__ == "__main__":
    unittest.main()
