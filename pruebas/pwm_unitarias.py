"""Comprueba el verificador PWM sin necesitar un binario de QEMU."""
import struct
import unittest

import qemu_prueba


class PruebaVerificadorPWM(unittest.TestCase):
    def setUp(self):
        self.tramas = [(1, 3, 0, struct.pack("<I", 3)),
                       (4, 3, 100, struct.pack("<BII", 13, 50000, 49987))]

    def test_acepta_pwm_y_limites_de_tolerancia(self):
        for frecuencia, ciclo in [(50000, 49987), (49500, 47000), (50500, 53000)]:
            with self.subTest(frecuencia=frecuencia, ciclo=ciclo):
                self.tramas[1] = (4, 3, 100, struct.pack("<BII", 13, frecuencia, ciclo))
                self.assertEqual(qemu_prueba.comprobar_pwm(self.tramas, "TALLER-OK pwm"), [])

    def test_rechaza_pin_frecuencia_o_ciclo_incorrectos(self):
        for pin, frecuencia, ciclo in [(12, 50000, 49987), (13, 49499, 49987),
                                      (13, 50501, 49987), (13, 50000, 46999),
                                      (13, 50000, 53001), (13, 50000, 0)]:
            with self.subTest(pin=pin, frecuencia=frecuencia, ciclo=ciclo):
                self.tramas[1] = (4, 3, 100, struct.pack("<BII", pin, frecuencia, ciclo))
                self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas, "TALLER-OK pwm"))

    def test_exige_marca_uart_y_pwm(self):
        self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas, ""))
        self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas[:1], "TALLER-OK pwm"))

    def test_exige_hola_v3_en_cabecera_y_carga(self):
        for cabecera, carga in [(2, 3), (3, 2), (3, 1)]:
            with self.subTest(cabecera=cabecera, carga=carga):
                self.tramas[0] = (1, cabecera, 0, struct.pack("<I", carga))
                self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas, "TALLER-OK pwm"))

    def test_rechaza_tramas_truncadas_sin_excepcion(self):
        self.tramas[0] = (1, 3, 0, b"\x03")
        self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas, "TALLER-OK pwm"))
        self.tramas[0] = (1, 3, 0, struct.pack("<I", 3))
        self.tramas[1] = (4, 3, 100, b"\x0d")
        self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas, "TALLER-OK pwm"))

    def test_rechaza_pwm_de_otra_version(self):
        self.tramas[1] = (4, 2, 100, self.tramas[1][3])
        self.assertTrue(qemu_prueba.comprobar_pwm(self.tramas, "TALLER-OK pwm"))


if __name__ == "__main__":
    unittest.main()
