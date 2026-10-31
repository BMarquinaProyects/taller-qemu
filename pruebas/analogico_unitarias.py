"""Comprueba las tramas y el lector analógico sin necesitar QEMU."""
import socket
import struct
import threading
import time
import unittest

import qemu_prueba


class PruebaAnalogico(unittest.TestCase):
    def test_trama_gpio_y_valor_little_endian(self):
        trama = qemu_prueba.trama_analogico(34, 2048)
        self.assertEqual(trama, struct.pack("<BBHQBH", 17, 3, 3, 0, 34, 2048))

    def leer(self, trozos, minimo, maximo, inicial=""):
        esperar = qemu_prueba.esperar_analogico
        lector, escritor = socket.socketpair()
        buf = [inicial]

        def enviar():
            with escritor:
                for trozo in trozos:
                    escritor.sendall(trozo)
                    time.sleep(0.01)

        hilo = threading.Thread(target=enviar)
        hilo.start()
        try:
            with lector:
                return esperar(lector, buf, minimo, maximo, 1)
        finally:
            hilo.join(timeout=2)

    def test_acepta_cero_y_limites_inclusivos(self):
        for valor, minimo, maximo in [(0, 0, 0), (1900, 1900, 2200),
                                      (2200, 1900, 2200), (4000, 4000, 4095),
                                      (4095, 4000, 4095)]:
            with self.subTest(valor=valor):
                self.assertTrue(self.leer([f"A={valor}\r\n".encode()], minimo, maximo))

    def test_recompone_linea_uart_fragmentada(self):
        self.assertTrue(self.leer([b"arranque\r\nA=2", b"04", b"8\r", b"\n"], 1900, 2200))

    def test_rechaza_numero_incompleto_y_prefijos(self):
        for texto in [b"A=2048", b"BA=2048\n", b"A=2048x\n", b"A=-2048\n"]:
            with self.subTest(texto=texto):
                self.assertFalse(self.leer([texto], 1900, 2200))

    def test_rechaza_fuera_de_margenes_y_sigue_leyendo(self):
        self.assertFalse(self.leer([b"A=1899\nA=2201\n"], 1900, 2200))
        self.assertTrue(self.leer([b"A=0\nA=2201\nA=2048\n"], 1900, 2200))
        self.assertFalse(self.leer([b"A=3999\nA=4096\n"], 4000, 4095))

    def test_descarta_muestras_de_lectura_anterior(self):
        lector, escritor = socket.socketpair()
        with lector, escritor:
            self.assertFalse(qemu_prueba.esperar_analogico(lector, ["A=2048\n"], 1900, 2200, 0))

    def test_no_acepta_continuacion_como_linea_nueva(self):
        self.assertFalse(self.leer([b"A=2048\n"], 1900, 2200, inicial="B"))
        self.assertTrue(self.leer([b"A=2048\nA=2048\n"], 1900, 2200, inicial="B"))


if __name__ == "__main__":
    unittest.main()
