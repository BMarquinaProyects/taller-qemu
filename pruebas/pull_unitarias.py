"""M-239: GPIO y emisor reales aislados, sin ejecutar QEMU."""
import shutil
import socket
import struct
import threading
import unittest
from pathlib import Path
from unittest import mock

from c3_modelos import compilar_ejecutar
import qemu_prueba

RAIZ = Path(__file__).resolve().parents[1]


class PruebaVerificadorPull(unittest.TestCase):
    def caso(self, chip):
        pines = [15, 16] if chip == "esp32" else [9 if chip == "c3" else 15]
        marca = "pull" if chip == "esp32" else f"{chip}_pull"
        texto = "PULL-LISTO\nPULL-SEMILLA=0\n"
        estados = [(0, 0)]
        for i, pin in enumerate(pines):
            prefijo = "" if i == 0 else f"GPIO{pin}-"
            texto += f"{prefijo}PULL-ARRIBA=1\n{prefijo}PULL-ABAJO=0\n{prefijo}SIN-PULL=0\n"
            estados += [(1 << pin, 0), (0, 1 << pin), (0, 0)]
        if chip == "esp32":
            texto += "GPIO34-PULLUP=0\n"
        texto += f"PULL-IMPUESTO-LISTO\nIMPUESTO=0\nTALLER-OK {marca}\n"
        estados += [(1 << pines[0], 0)]
        v = qemu_prueba.VERSION_PUENTE
        tramas = [(1, v, 0, struct.pack("<I", v))]
        tramas += [(9, v, i + 1, struct.pack("<QQ", *e)) for i, e in enumerate(estados)]
        return tramas, texto

    def test_tres_chips(self):
        for chip in ("esp32", "c3", "s3"):
            tramas, texto = self.caso(chip)
            self.assertEqual(qemu_prueba.comprobar_pull(tramas, texto, chip), [])

    def test_entrada_suelta_anuncia_v8_y_modos_previos_admiten_pull(self):
        self.assertEqual(qemu_prueba.trama_entrada(15, 2),
                         struct.pack("<BBHQBB", 16, 8, 2, 0, 15, 2))
        v = qemu_prueba.VERSION_PUENTE
        base = [(1, v, 0, struct.pack("<I", v)), (9, v, 1, bytes(16))]
        for chip in ("c3", "s3"):
            for tipo in (3, 5, 7):
                self.assertEqual(qemu_prueba.comprobar_tramas_periferico_soc(base, tipo, chip), [])
                self.assertTrue(qemu_prueba.comprobar_tramas_periferico_soc(
                    base + [(9, v, 2, b"\0")], tipo, chip))

    def test_rechaza_uart_y_mascaras_falsas(self):
        for chip in ("esp32", "c3", "s3"):
            tramas, texto = self.caso(chip)
            for malo in (texto.replace("SIN-PULL=0", "SIN-PULL=1"),
                         texto.replace("IMPUESTO=0", "IMPUESTO=1"), texto * 2,
                         texto.rstrip(), "prefijo" + texto,
                         texto.replace("PULL-ARRIBA=1", "PULL-ARRIBA=1basura")):
                self.assertTrue(qemu_prueba.comprobar_pull(tramas, malo, chip))
            v = qemu_prueba.VERSION_PUENTE
            for malas in (tramas[:1] + tramas[2:], tramas[:-1],
                          tramas + [tramas[-1]], tramas[:2],
                          tramas + [(9, v, 100, b"\0")],
                          tramas + [(9, v - 1, 100, bytes(16))],
                          tramas + [(9, v, 100, struct.pack("<QQ", 1 << 63, 0))],
                          tramas[:2] + list(reversed(tramas[2:]))):
                self.assertTrue(qemu_prueba.comprobar_pull(malas, texto, chip))

    def test_modos_inyectan_antes_de_autorizar_lecturas(self):
        for chip in ("esp32", "c3", "s3"):
            with self.subTest(chip=chip):
                self.modo_socket(chip)

    def modo_socket(self, chip):
        puente, externo = socket.socketpair()
        uart, firmware = socket.socketpair()
        errores = []
        tramas, texto = self.caso(chip)
        pines = [15, 16, 34] if chip == "esp32" else [9 if chip == "c3" else 15]

        def enviar_trama(trama):
            tipo, version, ns, carga = trama
            externo.sendall(qemu_prueba.CABECERA.pack(tipo, version, len(carga), ns) + carga)

        def recibir(sock, cantidad):
            datos = b""
            while len(datos) < cantidad:
                trozo = sock.recv(cantidad - len(datos))
                if not trozo:
                    raise EOFError("el modo cerró antes de inyectar")
                datos += trozo
            return datos

        def servidor():
            try:
                externo.settimeout(3)
                firmware.settimeout(3)
                for t in tramas[:2]:
                    enviar_trama(t)
                for b in b"PULL-LISTO\r\n":
                    firmware.sendall(bytes([b]))
                # Hay latidos pendientes de antes de inyectar: no son un acuse.
                for ns in (2, 3):
                    enviar_trama((8, 8, ns, b""))
                esperado = b"".join(qemu_prueba.trama_entrada(p, 0) for p in pines)
                self.assertEqual(recibir(externo, len(esperado)), esperado)
                self.assertEqual(recibir(firmware, 1), b"C")
                # El firmware confirma los ceros efectivos antes de soltar.
                firmware.sendall(b"PULL-SEMILLA=0\r\n")
                esperado = b"".join(qemu_prueba.trama_entrada(p, 2) for p in reversed(pines))
                self.assertEqual(recibir(externo, len(esperado)), esperado)
                self.assertEqual(recibir(firmware, 1), b"S")
                for t, v, ns, c in tramas[2:]:
                    enviar_trama((t, v, ns + 10, c))
                cuerpo = texto.split("PULL-SEMILLA=0\n", 1)[1].split("IMPUESTO=0", 1)[0]
                firmware.sendall(cuerpo.replace("\n", "\r\n").encode())
                esperado = qemu_prueba.trama_entrada(pines[0], 0)
                self.assertEqual(recibir(externo, len(esperado)), esperado)
                self.assertEqual(recibir(firmware, 1), b"I")
                final = "IMPUESTO=0" + texto.split("IMPUESTO=0", 1)[1]
                for b in final.replace("\n", "\r\n").encode():
                    firmware.sendall(bytes([b]))
            except BaseException as exc:
                errores.append(exc)
            finally:
                externo.close()
                firmware.close()

        proceso = mock.Mock(stdout=None)
        temporal = mock.Mock()
        lanzar = "lanzar" if chip == "esp32" else "lanzar_soc"
        hilo = threading.Thread(target=servidor, daemon=True)
        hilo.start()
        try:
            with mock.patch.object(qemu_prueba, lanzar, return_value=(temporal, proceso, 1, 2)), \
                 mock.patch.object(qemu_prueba, "conectar", side_effect=lambda p, _: puente if p == 2 else uart):
                fn = qemu_prueba.pull if chip == "esp32" else getattr(qemu_prueba, f"{chip}_pull")
                self.assertEqual(fn("qemu", "flash.bin"), 0)
            proceso.kill.assert_called_once()
            proceso.wait.assert_called_once()
            temporal.cleanup.assert_called_once()
        finally:
            puente.close()
            uart.close()
            hilo.join(timeout=4)
        self.assertFalse(hilo.is_alive())
        self.assertEqual(errores, [])


class PruebaPullModelo(unittest.TestCase):
    def compilar(self, codigo):
        cc = (shutil.which("cc") or shutil.which("gcc") or
              shutil.which("clang") or shutil.which("cl"))
        if cc is None:
            self.skipTest("compilador C no disponible")
        compilar_ejecutar(codigo, cc)

    def modelo(self, chip):
        fuente = (RAIZ / f"capas/hw/gpio/{chip}_gpio.c").read_text(encoding="utf-8")
        cab = (RAIZ / f"capas/include/hw/gpio/{chip}_gpio.h").read_text(encoding="utf-8")
        cab = "\n".join(l for l in cab.splitlines() if not l.startswith(("#include", "#pragma")))
        inicio = ("/* Taller: el GPIO del chip" if chip == "esp32" else
                  "void esp32c3_gpio_fijar_entrada" if chip == "esp32c3" else
                  "static bool esp32s3_gpio_pin_valido")
        modelo = fuente[fuente.index(inicio):fuente.index(f"static Property {chip}_gpio_properties")]
        plantilla = (RAIZ / "pruebas/pull_gpio_modelo.c").read_text(encoding="utf-8")
        armado = (RAIZ / "capas/include/hw/misc/taller_armado.h").read_text(encoding="utf-8")
        tren = (RAIZ / "capas/include/hw/misc/taller_tren.h").read_text(encoding="utf-8")
        pull = (RAIZ / "capas/include/hw/gpio/taller_pull.h").read_text(encoding="utf-8")
        codigo = plantilla.replace("/* CABECERA_REAL */", (armado + tren + pull + cab).replace("#pragma once", "")).replace(
            "/* MODELO_REAL */", modelo)
        self.compilar(f"#define CHIP_{chip.upper()} 1\n" + codigo)

    def test_clasico(self):
        self.modelo("esp32")

    def test_c3(self):
        self.modelo("esp32c3")

    def test_s3(self):
        self.modelo("esp32s3")

    def test_emisor(self):
        fuente = (RAIZ / "capas/hw/misc/taller_puente.c").read_text(encoding="utf-8")
        codigo = fuente[fuente.index("static void enviar_en("):fuente.index("/* TIEMPO")]
        codigo += fuente[fuente.index("static void enviar_pull("):
                         fuente.index("void taller_puente_disparo(")]
        codigo += fuente[fuente.index("void taller_puente_iniciar("):
                         fuente.index("bool taller_puente_activo(")]
        cab = (RAIZ / "capas/include/hw/misc/taller_puente.h").read_text(encoding="utf-8")
        cab = "\n".join(l for l in cab.splitlines() if not l.startswith(("#include", "#pragma")))
        plantilla = (RAIZ / "pruebas/pull_emisor.c").read_text(encoding="utf-8")
        self.compilar(plantilla.replace("/* PROTOCOLO_REAL */", cab).replace(
            "/* EMISOR_REAL */", codigo))


if __name__ == "__main__":
    unittest.main()
