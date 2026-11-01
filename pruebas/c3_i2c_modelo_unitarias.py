"""Compila el controlador I2C C3 real con un bus QEMU síncrono mínimo."""
import unittest

from c3_modelos import ejecutar_modelo


class PruebaModeloI2CC3(unittest.TestCase):
    def test_registros_fifo_irq_y_bus(self):
        ejecutar_modelo(self, "i2c", carpeta="i2c")


if __name__ == "__main__":
    unittest.main()
