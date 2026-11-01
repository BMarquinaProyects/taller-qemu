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
- GPIO del ESP32-C3 independiente del clásico, con 22 pines, registros OUT/ENABLE/IN,
  W1TS/W1TC, PAD_DRIVER en `0x74 + 4·n`, selección de salida conservada en
  `0x554 + 4·n` y trenes armados reutilizados (`hw/gpio/esp32c3_gpio.c` y cabecera).
- Máquina C3 conectada al chardev `taller-puente` (`hw/riscv/esp32c3.c`).
  El parser común entrega cargas a un receptor por máquina; el clásico conserva
  GPIO/ADC1/I2C y el C3 recibe GPIO, armados, ADC1 e I2C0. No se modifica la matriz IRQ C3:
  el modelo base ya conserva niveles, aplica OR y recalcula al reasignar.
- Compilación y empaquetado de Xtensa y RISC-V32 mediante `construir.sh`, con
  reglas Meson para el GPIO y el puente C3. Prueba Arduino e imagen completa de
  flash de 4 MiB, modo `c3_gpio`, unitarias del modelo real y despacho de entradas.
  IO_MUX, IRQ GPIO, RMT RX y SPI2 de usuario quedan pendientes;
  los controladores clásicos no se trasladan al C3.
- LEDC del ESP32-C3 (`hw/misc/esp32c3_ledc.c` y cabecera): cuatro timers y seis
  canales LS que mandan PWM al GPIO de su señal; ADC1 del C3
  (`hw/misc/esp32c3_saradc.c` y cabecera) de una lectura con valores inyectados
  por ANALOGICO. Conectados en `hw/riscv/esp32c3.c`; `esp32c3_gpio.c` resuelve
  la señal de salida de cada pin. Versión 0.9.0. I2C0 y RMT TX del C3 en la 0.10.0.
- I2C0 del ESP32-C3 (`hw/i2c/esp32c3_i2c.c` y cabecera): comandos, FIFO APB,
  actualización y reset FSM propios del C3; END, RSTART, NACK e IRQ. Reutiliza
  el esclavo `taller_i2c` del clásico en bus 0, con altas/registros desde el
  receptor C3 y escrituras completas hacia el puente. Kconfig selecciona I2C
  para el C3 y Meson incorpora controlador y espejo.
- RMT TX del ESP32-C3 (`hw/misc/esp32c3_rmt.c` y cabecera): dos TX con bloques
  de 48 palabras, lectura diferida de RAM, actualización, umbral ping-pong
  con contrapresión, fin e IRQ. Resuelve señales 51/52 mediante GPIO y emite
  los símbolos crudos con su tick y marca de inicio por el mismo puente RMT.
  Sin RX, DMA, portadora ni modo continuo.
- Pruebas Arduino C3 I2C/RMT, imágenes fusionadas de 4 MiB, verificadores
  puros y lanzadores por sockets; modelos reales aislados con adaptadores
  mínimos, también compilables con MSVC. `todas.sh` añade ambos modos C3.
  Este paso mantiene versión 0.9.0 y protocolo v7; compilación completa de
  QEMU y ejecución integrada quedan para la revisión en WSL.
- Puente v7 (`hw/misc/taller_puente.c`): latido TIEMPO cada 10 ms virtuales con un
  temporizador de `QEMU_CLOCK_VIRTUAL` y cadencia fija. Versión 0.8.1.
