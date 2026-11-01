"""Verificador PWM C3 y modelo C real, sin ejecutar QEMU."""
import struct
import unittest

import qemu_prueba
from c3_modelos import ejecutar_modelo

V = qemu_prueba.VERSION_PUENTE


class PruebaPwmC3(unittest.TestCase):
    def setUp(self):
        self.texto = "TALLER-OK c3_pwm\r\n"
        self.tramas = [(1, V, 0, struct.pack("<I", V))]
        for i, (frecuencia, ciclo) in enumerate(((50000, 49987), (50000, 250000),
                                                (100000, 250000), (100000, 0))):
            self.tramas.append((4, V, i + 1, struct.pack("<BII", 8, frecuencia, ciclo)))

    def comprobar(self, tramas=None, texto=None):
        fn = getattr(qemu_prueba, "comprobar_c3_pwm", None)
        self.assertTrue(callable(fn), "falta verificador c3_pwm")
        return fn(self.tramas if tramas is None else tramas,
                  self.texto if texto is None else texto)

    def test_frecuencia_ciclo_y_apagado_en_orden(self):
        self.assertEqual(self.comprobar(), [])

    def test_rechaza_pasos_perdidos_pin_erroneo_y_reordenacion(self):
        for tramas in (self.tramas[:-1], self.tramas[:2] + self.tramas[3:],
                       self.tramas[:1] + self.tramas[:0:-1],
                       [(t, v, n, bytes([13]) + c[1:] if t == 4 else c)
                        for t, v, n, c in self.tramas]):
            self.assertTrue(self.comprobar(tramas=tramas))

    def test_rechaza_uart_falsa_version_y_carga_rota(self):
        for texto in ("prefijoTALLER-OK c3_pwm\n", "TALLER-OK c3_pwm",
                      self.texto * 2, self.texto + "TALLER-FALLO c3_pwm\n"):
            self.assertTrue(self.comprobar(texto=texto))
        for trama in ((4, V, 8, b"\x08"),
                      (4, 6, 8, struct.pack("<BII", 8, 50000, 50000)),
                      (4, V, 8, struct.pack("<BII", 22, 50000, 50000)),
                      (4, V, 8, struct.pack("<BII", 8, 0, 1)),
                      (4, V, 8, struct.pack("<BII", 8, 50000, 1000001))):
            self.assertTrue(self.comprobar(tramas=self.tramas + [trama]))

    def test_modelo_real(self):
        ejecutar_modelo(self, "ledc")


if __name__ == "__main__":
    unittest.main()
