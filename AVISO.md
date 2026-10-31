# Aviso de origen y cambios

Este repositorio contiene modificaciones del **QEMU de Espressif** (`espressif/qemu`,
etiqueta `esp-develop-9.2.2-20260417`), que a su vez deriva de QEMU (https://www.qemu.org).
Licencia: GNU GPL versión 2 (ver `LICENSE`), igual que el QEMU original.

Los archivos de `capas/` sustituyen a los del código base al compilar. El primer commit
los contiene sin modificar; el historial de Git muestra cada cambio.

Cambios de Taller (resumen):
- GPIO del ESP32 con salidas, habilitación y lectura reales (`hw/gpio/esp32_gpio.c`).
- Dispositivo "puente" que envía a un programa externo, por un `chardev`, los cambios de
  GPIO y los bytes de SPI con marca de tiempo virtual (`hw/misc/taller_puente.c`).
- Periférico SSI en SPI2/SPI3 conectado al puente (`hw/ssi/esp32_spi.c`, `hw/xtensa/esp32.c`).
