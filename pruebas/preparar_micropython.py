"""Verifica firmware oficial y lo coloca en 0x1000 de una flash de 4 MiB a FF.

python3 pruebas/preparar_micropython.py /ruta/ESP32_GENERIC-20260824-v1.29.0.bin
Sin argumento, descarga el firmware oficial. La salida no se incorpora a Git.
"""
import hashlib
import sys
import urllib.request
from pathlib import Path

URL = "https://micropython.org/resources/firmware/ESP32_GENERIC-20260824-v1.29.0.bin"
SHA256 = "e67ad6015a0a504c1fec9aa9bbf589d0432ed28e62546f4f8dd8a147f8bd95f6"


def preparar(datos: bytes) -> bytes:
    if hashlib.sha256(datos).hexdigest() != SHA256:
        raise ValueError("SHA-256 del firmware MicroPython distinto del esperado")
    imagen = bytearray(b"\xff" * (4 * 1024 * 1024))
    if len(datos) > len(imagen) - 0x1000:
        raise ValueError("el firmware excede la flash de 4 MiB")
    imagen[0x1000:0x1000 + len(datos)] = datos
    return bytes(imagen)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        datos = Path(sys.argv[1]).read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=60) as respuesta:
            datos = respuesta.read()
    salida = Path(__file__).with_name("micropython.flash.bin")
    salida.write_bytes(preparar(datos))
    print(f"OK {salida}: 4 MiB, firmware verificado en 0x1000")
