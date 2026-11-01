"""Programa ejecutado por qemu_prueba.py mediante el REPL raw paste."""
from machine import SPI, Pin

spi = SPI(2, baudrate=20_000_000, sck=Pin(18), mosi=Pin(23))
spi.write(b"abcd")
spi.write(bytes(range(256)) * 160)
recibido = spi.read(4)
print("MISO=" + recibido.hex())
assert recibido == b"\xff" * 4, recibido
spi.deinit()
print("TALLER-OK spi_dma")
