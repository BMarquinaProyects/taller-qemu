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
- Matriz S3 (`hw/xtensa/esp32s3_intc.c` y cabecera): originales del tarball
  de Espressif, con niveles por fuente y CPU en arrays (E-039/E-045), OR de
  fuentes compartidas y recálculo de líneas vieja/nueva al reasignar. Permite
  que `esp_intr_enable` despierte la ISR SPI2 con `trans_done` ya pendiente.
  Prueba real aislada en `pruebas/s3_intmatrix_unitarias.py`, ambas CPU,
  fuentes 29/61/90+ y reset; falla con el original y pasa con la corrección.
- GPIO del ESP32-C3 independiente del clásico, con 22 pines, registros OUT/ENABLE/IN,
  W1TS/W1TC, PAD_DRIVER en `0x74 + 4·n`, selección de salida conservada en
  `0x554 + 4·n` y trenes armados reutilizados (`hw/gpio/esp32c3_gpio.c` y cabecera).
- Máquina C3 conectada al chardev `taller-puente` (`hw/riscv/esp32c3.c`).
  El parser común entrega cargas a un receptor por máquina; el clásico conserva
  GPIO/ADC1/I2C y el C3 recibe GPIO, armados, ADC1 e I2C0. La matriz IRQ C3 base ya
  conserva niveles, aplica OR y recalcula al reasignar (ver abajo el arreglo de v0.12.1).
- Compilación y empaquetado de Xtensa y RISC-V32 mediante `construir.sh`, con
  reglas Meson para el GPIO y el puente C3. Prueba Arduino e imagen completa de
  flash de 4 MiB, modo `c3_gpio`, unitarias del modelo real y despacho de entradas.
  IO_MUX, IRQ GPIO y RMT RX quedan pendientes;
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
- SPI2/GPSPI2 del C3 (`hw/ssi/esp32c3_spi2.c` y cabecera), independiente del
  SPI1 de flash: W0–W15, UPDATE/USR, MS_DLEN, fases de comando/dirección,
  RAW R/W, ENA/ST/CLR y fin con IRQ. CPU y GDMA TX/RX sobre los canales
  existentes de Espressif, descriptores SRAM C3 de 12 bytes, length/size,
  CHECK_OWNER, OUT_AUTO_WRBACK, EOF, errores y detección de ciclos.
  TX parcial no devuelve OWNER ni genera OUT_DONE/EOF anticipados; los
  finales RAW se conservan hasta CLR o escritura explícita, incluso entre modos.
  MISO FF; `taller.spi` y `taller_puente_spi_fin` emiten en **bus 2** con el
  mismo protocolo v7 y troceo del clásico. Conectado en `hw/riscv/esp32c3.c`
  y Meson, sin trasladar el SPI clásico ni modificar el SPI1 de flash.
- Correcciones de traducción GDMA C3 (`hw/dma/esp32c3_gdma.c`): IRQ por
  canal con stride 0x10, cinco eventos IN y cuatro OUT, STOP/reset.
  OR de IRQ IN/OUT por canal en la máquina; Kconfig selecciona OR_IRQ.
  SPI2 requiere selección de periférico y START; accede al AddressSpace DRAM
  con direcciones absolutas (como el motor GDMA de Espressif). El motor GDMA genérico
  permanece como en Espressif; se conserva el recálculo al reasignar.
- Prueba Arduino `c3_spi`, imagen fusionada de 4 MiB, modo en
  `qemu_prueba.py` y llamada en `todas.sh`. Patrón MOSI exacto de 40968 bytes
  en bus 2, Arduino CPU y spi_master con GDMA e ISR; MISO ffffffff.
  Verificador puro y modelos reales SPI2/GDMA/emisor aislados compilables
  con GCC/Clang o MSVC; regresiones de rangos SRAM, OWNER, RX/TX parciales
  y segmento pendiente. 101 unitarias pasan en WSL sin omisiones, y se
  compilan aisladamente SPI2, GDMA y máquina C3 con cabeceras reales y -Werror.
  Sin temporización eléctrica, PSRAM, esclavo,
  cadenas circulares/restart ni buffers SCT; probado en QEMU con `c3_spi`
  (21 modos de `todas.sh`). Versión 0.12.0, puente v7.
- Puente v7 (`hw/misc/taller_puente.c`): latido TIEMPO cada 10 ms virtuales con un
  temporizador de `QEMU_CLOCK_VIRTUAL` y cadencia fija. Versión 0.8.1.
- GPIO propio del ESP32-S3 (`hw/gpio/esp32s3_gpio.c` y cabecera), basado en
  la capa C3 y el modelo mínimo de Espressif: dos bancos hasta GPIO48,
  hueco 22..25, W1TS/W1TC, PAD_DRIVER, matriz de salida de 9 bits y trenes.
  La lógica compartida de trenes admite 49 índices; cada chip valida sus pines.
- Máquina S3 de Espressif trasladada a `capas/hw/xtensa/esp32s3.c`, con receptor
  propio de ENTRADA, ENTRADA_ARMADA y ANALOGICO y conexión al mismo puente v7.
  LEDC C3 reutilizado mediante parámetros de canales/señales y callback GPIO:
  seis canales LS y señales 45..50 en C3, ocho y señales 73..80 en S3.
- ADC1 S3 propio (`hw/misc/esp32s3_saradc.c` y cabecera), diez canales en
  GPIO1..10 con valores crudos inyectados. Arduino 3.3.12 usa el controlador
  RTC SENS para una lectura, distinto del APB_SARADC C3. Sin ADC2 ni DMA.
- Sketches e imágenes Arduino S3 de 4 MiB, modos `s3_gpio`, `s3_pwm` y
  `s3_analogico` y entradas en `todas.sh`. Verificadores/lanzadores compartidos
  con C3 y pruebas aisladas de modelos, receptor y sockets. GPIO del clásico
  mantiene su límite de 40 índices y C3 el de 22. IO_MUX, IRQ GPIO
  siguen pendientes. SPI2 se incorpora en el paso descrito abajo (v0.14.0).
- I2C0 del S3 conectado al modelo compartido C3 (`hw/i2c/esp32c3_i2c.c`),
  fuente IRQ 42 y espejo Taller de bus 0 antes de HOLA. Receptor S3 para
  I2C_DISPOSITIVO(18)/I2C_REGISTROS(19); emite I2C_ESCRITURA(5) como C3.
  Mapa y 314 campos numéricos comunes contrastados con Arduino 3.3.12 / IDF 5.5;
  diferencias de nombres y límites documentadas en `docs/esp32s3-i2c-rmt.md`.
- RMT TX C3/S3 compartido (`hw/misc/esp32c3_rmt.c` y cabecera), mapa S3
  mediante propiedad, cuatro TX y cuatro RX sin recepción, 48 palabras por
  bloque, RAM S3 desde +0x800 (C3 +0x400), préstamo entre TX, wrap y recarga
  ping-pong, IRQ 40 y señales 81..84.
  Callback GPIO por máquina; RMT(7) conserva tick, símbolos y sello inicial.
  Sin DMA, RX, portadora, sincronización ni modo continuo. La matriz Xtensa S3
  usa índices y no necesita la corrección BIT_ULL del C3 para Windows.
- Sketches `s3_i2c` (SDA8/SCL9) y `s3_rmt` (GPIO10, 8/30 LED GRB),
  imágenes DIO de 4 MiB, modos y verificadores puros en `qemu_prueba.py`,
  entradas en `todas.sh` y pruebas reales aisladas C3/S3, parser y sockets.
  Versión 0.13.0 y protocolo v7; 23 modos de `todas.sh` en Linux.
- Matriz de interrupciones del C3 (`hw/riscv/esp32c3_intmatrix.c`), v0.12.1: `BIT()`
  pasa a `BIT_ULL()`. `BIT(n)` es `1UL << n` y en Windows `long` tiene 32 bits, así que
  las fuentes 32..61 pisaban a las 0..29: el I2C0 (29) activaba también
  CACHE_CORE0_ACS (61) y el firmware caía en «Cache error». Prueba aislada en
  `pruebas/c3_intmatrix_unitarias.py`.

- SPI2/FSPI maestro S3: reutiliza `hw/ssi/esp32c3_spi2.c` con tipo derivado
  `esp32s3.spi2`, cinco canales GDMA, SRAM 0x3FC88000..0x3FD00000,
  INT_SET +0x44 W1S y SPI_DATE S3. CPU W0–W15, UPDATE/USR, DLEN de 18 bits,
  fases de comando/dirección, MISO FF, IRQ SPI2 y descriptores TX/RX de
  12 bytes con OWNER/EOF, errores y recorrido acotado. Direcciones dma_as
  absolutas (E-046), también en dobles; sin máscaras long (E-045).
- Máquina S3 conecta SPI2 en 0x60024000 al GDMA y a `taller.spi` bus 2;
  Meson compila el motor compartido en ambos chips. SPI1 conserva flash/PSRAM.
  `hw/dma/esp32s3_gdma.c` conserva traductor y mapa nativos de Espressif,
  con pulsos STOP/reset y PARK al nuevo START; cabecera S3 copiada del árbol
  base sin cambios para pruebas reproducibles. No cambia el motor genérico.
- Sketch e imagen DIO S3 de 4 MiB con Arduino 3.3.12, modo `s3_spi` y
  verificador puro (40968 bytes MOSI exactos por bus 2, lectura DMA FF),
  pruebas de sockets y modelos SPI2/GDMA/emisor reales compilados aislados.
  `todas.sh` añade el modo con finales LF. Documentación del contraste C3/S3
  y límites en `docs/esp32s3-spi.md`. Sin temporización eléctrica, SPI3,
  orden de bits en hilo, octal/DTR, esclavo, PSRAM DMA, circular ni SCT.
  Versión 0.14.0, protocolo v7. 23 modos de `todas.sh` en Linux, `s3_spi` incluido.
- SPI1 (flash) del S3 (`hw/ssi/esp32s3_spi.c`), v0.15.0: en las lecturas «I/O» los ciclos
  dummy llevan 2 (DIO) o 4 (QIO) bits por ciclo; `m25p80` los cuenta en bytes. La copia de
  Espressif los convertía como de una sola línea y, en QIO con el IS25LP128 (flash de 16 MiB),
  cada lectura salía desplazada 2 bytes. Prueba `s3_flash_qio` y `s3_flash_qio_unitarias.py`.
