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
- RMT TX del ESP32 clásico, con RAM de símbolos, umbral ping-pong, tiempo virtual
  e IRQ; puente v6 con trenes RMT completos (`hw/misc/esp32_rmt.c`).
- SPI DMA TX/RX para SPI2/HSPI y SPI3/VSPI: registros DMA, cadenas de
  descriptores en SRAM, devolución de owner, EOF, errores e IRQ separadas de
  trans_done. DPORT conserva la selección de canal. Lógica independiente en
  `include/hw/ssi/taller_spi_dma.h`, adaptador en `hw/ssi/esp32_spi.c` y
  selección en `hw/misc/esp32_dport.c`. Versión 0.7.0, protocolo del puente v6.
- Prueba MicroPython v1.29.0 mediante REPL raw paste, con verificación exacta
  de 40 960 bytes y recepción MISO. El firmware externo no se distribuye aquí.
- Matriz de interrupciones (`hw/xtensa/esp32_intc.c`): nivel por fuente y líneas como OR de sus fuentes, recalculadas al reasignar.
