"""Comprueba las tramas I2C y el verificador de humo sin ejecutar QEMU."""
import struct
import unittest

import qemu_prueba


class PruebaI2C(unittest.TestCase):
    def setUp(self):
        self.texto = "ENCONTRADO 0x3C\r\nLEIDO=60,61\r\nTALLER-OK i2c\r\n"
        self.tramas = [(1, 4, 0, struct.pack("<I", 4)),
                       (5, 4, 100, b"\x00\x3c\x00\xaf")]

    def comprobar(self, tramas=None, texto=None):
        comprobar = getattr(qemu_prueba, "comprobar_i2c", None)
        self.assertTrue(callable(comprobar), "falta el verificador I2C")
        return comprobar(self.tramas if tramas is None else tramas,
                         self.texto if texto is None else texto)

    def test_acepta_escaneo_lectura_y_escritura(self):
        self.assertEqual(self.comprobar(), [])

    def test_rechaza_otras_direcciones_duplicados_y_escaneo_vacio(self):
        for escaneo in ["", "ENCONTRADO 0x48\n", "ENCONTRADO 0x3C\n" * 2,
                        "ENCONTRADO 0x3C\nENCONTRADO 0x48\n",
                        "ENCONTRADO 0x3C basura\n"]:
            with self.subTest(escaneo=escaneo):
                self.assertTrue(self.comprobar(texto=escaneo + "LEIDO=60,61\nTALLER-OK i2c\n"))

    def test_exige_lectura_y_marca_completas(self):
        for antes, despues in [("LEIDO=60,61", "LEIDO=60,00"),
                               ("LEIDO=60,61", "LEIDO=60,61FF"),
                               ("TALLER-OK i2c", "TALLER-FALLO i2c"),
                               ("LEIDO=60,61", "PREFIJOLEIDO=60,61")]:
            with self.subTest(despues=despues):
                self.assertTrue(self.comprobar(texto=self.texto.replace(antes, despues)))

    def test_exige_hola_v4_y_escritura_exacta(self):
        for tramas in [[], self.tramas[:1], self.tramas[1:],
                       [(1, 3, 0, struct.pack("<I", 3)), self.tramas[1]],
                       [(1, 4, 0, b"\x04"), self.tramas[1]],
                       [(1, 3, 0, struct.pack("<I", 4)), self.tramas[1]]]:
            with self.subTest(tramas=tramas):
                self.assertTrue(self.comprobar(tramas=tramas))
        for carga in [b"", b"\x00\x3c\xaf", b"\x01\x3c\x00\xaf",
                      b"\x00\x48\x00\xaf", b"\x00\x3c\x00\xaf\x00"]:
            with self.subTest(carga=carga):
                self.assertTrue(self.comprobar(tramas=[self.tramas[0], (5, 4, 100, carga)]))
        self.assertTrue(self.comprobar(tramas=[self.tramas[0], (5, 3, 100, self.tramas[1][3])]))

    def test_tramas_entrantes_v4(self):
        dispositivo = getattr(qemu_prueba, "trama_i2c_dispositivo", None)
        registros = getattr(qemu_prueba, "trama_i2c_registros", None)
        self.assertTrue(callable(dispositivo) and callable(registros), "faltan constructores I2C")
        self.assertEqual(dispositivo(0, 0x3c, True),
                         struct.pack("<BBHQ", 18, 4, 3, 0) + b"\x00\x3c\x01")
        self.assertEqual(registros(1, 0x76, 0xd0, b"\x60\x61"),
                         struct.pack("<BBHQ", 19, 4, 5, 0) + b"\x01\x76\xd0\x60\x61")
        self.assertEqual(len(registros(0, 0x3c, 0, bytes(240))), 255)
        with self.assertRaises(ValueError):
            registros(0, 0x3c, 0, bytes(241))

    def test_pwm_existente_acepta_v4(self):
        tramas = [self.tramas[0], (4, 4, 100, struct.pack("<BII", 13, 50000, 50000))]
        self.assertEqual(qemu_prueba.comprobar_pwm(tramas, "TALLER-OK pwm"), [])


if __name__ == "__main__":
    unittest.main()
