# ESP32-S3: SPI2/FSPI de usuario y GDMA

Fase 6d sobre v0.13.0, puente v7, sin cambio de VERSION ni workflows.
SPI1 conserva flash/PSRAM. SPI2 maestro usa el motor
`capas/hw/ssi/esp32c3_spi2.c`, parametrizado mediante el tipo QOM derivado
`esp32s3.spi2`; su cabecera específica solo da nombre y bus del puente.

## Contraste C3/S3

Fuentes principales: cabeceras instaladas por Taller en
`%APPDATA%/dev.taller.app/arduino/data/packages/esp32/tools/esp32s3-libs/3.3.12/include`,
y sus equivalentes C3. Se leyeron `soc/spi_reg.h`, `spi_struct.h`,
`gdma_reg.h`, `gdma_struct.h`, `soc.h`, `gpio_sig_map.h` y
`hal/spi_ll.h`, `gdma_ll.h`. Corroboración pública en
[HAL SPI S3 de IDF 5.5](https://github.com/espressif/esp-idf/blob/v5.5/components/hal/esp32s3/include/hal/spi_ll.h)
y [HAL GDMA S3](https://github.com/espressif/esp-idf/blob/v5.5/components/hal/esp32s3/include/hal/gdma_ll.h).

| Elemento | C3 | S3 |
|---|---|---|
| SPI2 base / W0..W15 | `0x60024000` / `+0x98..0xd4` | Igual |
| CMD | `+0x00`, UPDATE bit 23, USR bit 24 | Igual |
| USER / USER1 / USER2 | `+0x10/0x14/0x18` | Igual para maestro de una línea |
| MS_DLEN | `+0x1c`, 18 bits, bits de datos menos uno | Igual: máximo 32768 bytes |
| DMA_CONF | `+0x30`, RX bit 27, TX bit 28 | Igual |
| IRQ ENA/CLR/RAW/ST | `+0x34/0x38/0x3c/0x40`, fin bit 12, segmento bit 13 | Igual |
| Lanzar primera ISR desde HAL | RAW R/W | **INT_SET `+0x44`, W1S**; conserva RAW pendiente y se lee como cero |
| CTRL.WR_BIT_ORDER / RD_BIT_ORDER | bit 26 / bit 25 | Campos de dos bits en 26..25 / 24..23; se conservan, sin modelar orden eléctrico |
| Extensiones SPI | Sin octal/DTR | Octal, DTR, DQS y retardos extra; fuera de esta capa |
| SPI_DATE al reset | `0x02007220` | `0x02101190` |
| Señales FSPICLK / FSPID(MOSI) | 63 / 65 | **101 / 103**; FSPIQ=102, CS0=110 |
| Canales GDMA | 3 | **5**, IN y OUT independientes |
| SRAM aceptada por el modelo | `[0x3FC80000,0x3FCE0000)` | **`[0x3FC88000,0x3FD00000)`**, SOC_DMA_LOW/HIGH |
| Registros GDMA | IRQ combinada, configuración desde `+0x70` | Mapa nativo: canal `n*0xc0`, OUT `+0x60`; IRQ RAW/ST/ENA/CLR `+0x08/0x0c/0x10/0x14`, LINK `+0x20`, PERI_SEL `+0x48` |

La máquina S3 ya tenía GDMA de Espressif conectado en `0x6003f000`, con
cinco IRQ IN y cinco OUT hacia su matriz. Se conserva ese cableado. SPI2
se conecta a `ETS_SPI2_INTR_SOURCE` y al mismo `ESPGdmaState`, con
`taller.spi` en **bus 2**, igual que C3. El objeto global Arduino `SPI`
es `SPIClass(FSPI)` en S3.

## Transferencias y descriptores

UPDATE no transmite. USR termina dentro del acceso MMIO, completa IRQ y
vacía transacción hacia el puente. W0–W15 cubren hasta 64 bytes por CPU;
highpart empieza en W8. Comando y dirección se emiten cuando USER los
habilita. MISO devuelve FF, incluidos los buffers RX de GDMA. RX sin TX
genera bytes MOSI cero; no reutiliza contenido viejo de W0.

GDMA selecciona periférico SPI2=0 **y** START de su dirección: IN bit 22,
OUT bit 21; rechaza STOP y reset. Recorre descriptores little endian de
12 bytes con size/length/OWNER/EOF, límites SRAM, detección de ciclos,
writeback OUT opcional, devolución RX y eventos DONE/EOF/error. El fin
consume START y deja PARK; otro USR sin nuevo START no repite MOSI.

**E-046:** las operaciones sobre `dma_as` reciben direcciones absolutas.
En S3 la raíz es la DRAM directa colocada en sys_mem; en C3 es su alias.
No se resta la base. Los dobles aislados exigen direcciones absolutas.
**E-045:** no se incorporan máscaras `long` ni desplazamientos `1UL`;
los bits que superen 31 requieren `BIT_ULL()` o `uint64_t`.

El traductor `capas/hw/dma/esp32s3_gdma.c` conserva el mapa nativo de
Espressif. Añade pulsos STOP y cancelación de START al reset, como C3:
el HAL hace read/modify/write al arrancar de nuevo y un STOP retenido
bloquearía el siguiente bloque. Se conserva AUTO_RET de IN y se baja
PARK al nuevo START. No se modifica `esp_gdma.c`, compartido con AES/SHA.
Cabecera GDMA S3 copiada sin cambios del árbol base para banco reproducible.

La matriz Xtensa S3 también requiere la corrección E-039 del clásico:
IDF deja `trans_done` alto y habilita la interrupción reasignando su fuente.
El original no guardaba niveles ni recalculaba al reasignar; tampoco hacía
OR de fuentes compartidas. Por eso la ISR no lanzaba el primer GDMA y
`spi_device_transmit` quedaba esperando. `capas/hw/xtensa/esp32s3_intc.c`
y su cabecera proceden del tarball original de Espressif; añaden arrays
de niveles por fuente y CPU, OR y recálculo de la línea vieja y la nueva.
Se mantienen las 512 entradas por CPU que reserva el mapa base, sin máscaras
de `long` ni límite de 64 fuentes. El reset borra niveles y baja salidas.
SPI2 ya recalcula IRQ al escribir RAW/ENA/SET/CLR; el GDMA genérico también
recalcula con RAW/ENA/CLR. No necesitan otro cambio por este bloqueo.

## Prueba reproducible

`pruebas/s3_spi_prueba/s3_spi_prueba.ino`, SCK12/MOSI11/MISO13:

1. Arduino `SPI.beginTransaction`, `transfer('a')`, `writeBytes("bcd")`.
2. Libera SPI e inicializa `spi_master`, SPI2_HOST, SPI_DMA_CH_AUTO.
3. Dos `spi_device_transmit` mediante ISR: 32768 y 8192 bytes, patrón
   `uint8_t(i)` en cada bloque.
4. Lectura DMA de cuatro bytes: exige `S3-SPI-MISO=ffffffff` y
   `TALLER-OK s3_spi`, líneas completas, únicas y ordenadas.

`comprobar_s3_spi` es puro: exige HOLA v7, sobre y GPIO válidos para S3,
tiempos no decrecientes y **40968 bytes MOSI exactos en orden por bus 2**:
`abcd`, patrón de 40960 bytes y cuatro ceros RX. Acepta fragmentación del
puente; rechaza omisiones, repeticiones, alteraciones y otros buses.

```sh
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa s3_spi pruebas/s3_spi_prueba.flash.bin
```

El banco SPI compila el motor real compartido, sustituyendo solo servicios
QEMU; S3 añade los cinco canales, extremos de SRAM, SPI_DATE e INT_SET.
GDMA compila traductor y campos de su cabecera real: cinco canales, mapa
nativo, IRQ separadas, STOP/START/reset. También se compila emisor real del
puente y se prueba lanzador mediante sockets. MSVC usa C11, /W4 y /WX;
GCC/Clang usan -Wall -Wextra -Werror. Regresión C3 se ejecuta por separado.

Imagen fusionada **4194304 bytes**, Arduino CLI Taller **1.5.1**, core
**3.3.12**, FQBN
`esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default`.
Bootloader S3 DIO en 0x0, particiones 0x8000, boot_app0 0xe000, app 0x10000.
SHA-256: `cea4c292ce30fca58fe927583059636d2e678f63fc619d6c4edcf697d500aa07`.

## Límites y verificación

Sin tiempo eléctrico de SPI/DMA, orden de bits en hilo, dual/quad/octal,
DTR, esclavo, SPI3 de usuario, PSRAM DMA, transferencia circular/continua,
recarga por RESTART ni interpretación de buffers de configuración SCT.
Los registros de reloj, modo y CS se conservan; no generan pulsos GPIO.
El puente expone bytes MOSI por transacción, con su troceo habitual de 4096.
Esto permite la ruta CPU de Adafruit_ST7735 sobre SPI y la ruta GDMA de
spi_master; queda pendiente probar pantalla real en Taller.

Tras la corrección de matriz, WSL permite ejecutar comprobaciones:
**137 unitarias correctas, sin omisiones**. Las seis nuevas en
`pruebas/s3_intmatrix_unitarias.py` compilan cabecera y funciones reales
IRQ/MMIO/reset con dobles mínimos; fallan por aserciones con el original
del tarball y pasan con el arreglo. Cubren nivel alto antes de asignar,
OR compartido, reasignación en ambas CPU, fuentes 29/61/90+ independientes,
reset y fuente pendiente cuando una CPU no tiene salida conectada.

GCC genera objetos aislados de **los archivos completos** matriz S3, SPI2,
GDMA S3 y máquina S3, con `-Werror`, cabeceras y opciones reales del
`compile_commands.json` Linux. Fuentes, cabeceras de capas y objetos quedan
en este worktree; el árbol base solo se lee. No se enlaza QEMU completo.
La imagen conserva la compilación anterior con herramientas Arduino.
En la revisión (v0.14.0) se compiló QEMU completo: `todas.sh` pasa sus 23 modos en Linux,
`s3_spi` incluido (40 968 bytes MOSI exactos por el bus 2 y MISO `ffffffff`), y 137 unitarias.
