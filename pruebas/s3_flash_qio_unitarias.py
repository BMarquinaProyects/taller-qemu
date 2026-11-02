"""Verificador puro de s3_flash_qio: acepta la lectura correcta y rechaza la desplazada 2 bytes."""
import unittest

from qemu_prueba import comprobar_s3_flash_qio

BUENO = """BORRADO err=0 primeros=ff ff ff ff
ESCRITO err=0 primeros=a0 a1 a2 a3 igual=1
TAMANO=4194304 FISICO=16777216 ID=9d6018
TALLER-OK flash
"""


class PruebaS3FlashQio(unittest.TestCase):
    def test_lectura_correcta(self):
        self.assertEqual(comprobar_s3_flash_qio(BUENO), [])

    def test_desfase_de_dos_bytes(self):
        malo = BUENO.replace("primeros=ff ff ff ff", "primeros=00 00 ff ff").replace(
            "primeros=a0 a1 a2 a3 igual=1", "primeros=00 00 a0 a1 igual=0")
        self.assertEqual(len(comprobar_s3_flash_qio(malo)), 2)

    def test_sin_qio(self):
        self.assertIn("no se activó el modo QIO",
                      comprobar_s3_flash_qio("E (44) qio_mode: Failed to set QIE bit\n" + BUENO))


if __name__ == "__main__":
    unittest.main()
