# taller-qemu

QEMU de Espressif para ESP32 con periféricos extra (GPIO real, ADC1, SPI DMA TX/RX, RMT TX, puente de GPIO/SPI/PWM/analógico/I2C/RMT y entradas temporizadas), ESP32-C3 con GPIO, LEDC, ADC1, I2C0, RMT TX y SPI2 CPU/GDMA, y ESP32-S3 con GPIO, LEDC y ADC1, todos conectados al puente, que usa
**Taller**, un simulador de placas y componentes. Ver `AVISO.md` para el origen y los cambios.

## Compilar

Linux / WSL (Ubuntu 24.04):

```
sudo apt install build-essential ninja-build pkg-config python3-venv flex bison \
  libglib2.0-dev libpixman-1-dev libslirp-dev libgcrypt20-dev
TRABAJO=~/qemu-trabajo ./construir.sh
```

Windows: MSYS2 **UCRT64** (MINGW64 ya no trae `libslirp`) con los paquetes `mingw-w64-ucrt-x86_64-{gcc,glib2,pixman,libslirp,libgcrypt,ninja,meson,python,pkgconf}`.
Ver `.github/workflows/qemu.yml`.

`construir.sh` genera y empaqueta **qemu-system-xtensa** y **qemu-system-riscv32**
(también en Windows). Si el directorio de trabajo tenía solo Xtensa, se vuelve a
configurar para añadir RISC-V32; esa primera compilación necesita generar sus objetos.

## ESP32-C3: GPIO, LEDC, ADC1, I2C0, RMT TX, SPI2 y puente (fase 6d)

Usar **qemu-system-riscv32 -machine esp32c3**. El mismo chardev
`taller-puente` activa HOLA y las tramas GPIO, SPI, PWM, I2C_ESCRITURA, RMT y TIEMPO
del protocolo **v7**. Bit n significa GPIO n también en el C3. ENTRADA,
ENTRADA_ARMADA, ANALOGICO, I2C_DISPOSITIVO e I2C_REGISTROS llegan a los modelos
de la máquina seleccionada mediante un receptor propio; las rutas del ESP32
clásico se conservan. El C3 admite solamente el bus I2C **0**.

El modelo C3 tiene **22 GPIO (0..21)**. OUT (`0x04`), ENABLE (`0x20`),
IN (`0x3c`) y sus W1TS/W1TC usan solamente esos 22 bits. `GPIO_PINn_REG`
está en **`0x74 + 4·n`**: conserva lo escrito y su bit 2, PAD_DRIVER,
activa drenador abierto. IN devuelve el nivel propio en salida push-pull,
el externo en entrada y el AND de ambos en salida de drenador abierto.
`FUNCn_OUT_SEL_CFG` se conserva en **`0x554 + 4·n`**, con señal software
128 al reset. Los offsets y PAD_DRIVER se contrastaron con `gpio_reg.h`,
`gpio_ll.h` y `gpio_sig_map.h` del core Arduino ESP32 3.3.12.

Los trenes reutilizan `taller_armado.h`: ocho ranuras, hasta 200 pasos,
temporizadores virtuales, DISPARO y reglas de rearme/reset del clásico.
Ambos pines del programa deben pertenecer a `0..21`; los demás se ignoran
antes de modificar las ranuras. ENTRADA también ignora pines ajenos al C3.

Inventario de la máquina base `esp-develop-9.2.2-20260417` y trabajo pendiente:

| Pieza | Estado en C3 |
|---|---|
| GPIO | Salida, entrada externa, drenador abierto y trenes conectados al puente. Sin IRQ GPIO ni filtros. |
| IO_MUX / matriz GPIO | IO_MUX no está emulado: lecturas cero y escrituras ignoradas. PINn y selección de salida conservan valores; LEDC y RMT resuelven el GPIO de su señal de salida, sin inversión. La entrada externa empieza alta, como en el clásico; INPUT_PULLUP funciona con ese reposo, sin simular resistencias ni INPUT_PULLDOWN. |
| Matriz IRQ | El original ya guarda `irq_levels`, aplica OR por línea y recalcula líneas vieja/nueva al reasignar. Conserva prioridad, umbral y habilitación; no tiene el fallo corregido del clásico. La selección de IRQ por flanco no está emulada. No se sustituye este archivo. |
| UART y temporizadores | Dos UART con FIFO/timeout/IRQ; dos TIMG con T0 y watchdog; SYSTIMER con dos contadores y tres alarmas. Se conservan los modelos de Espressif. |
| LEDC | `hw/misc/esp32c3_ledc.c`: cuatro timers y seis canales LS (registros de `soc/ledc_reg.h` del C3). Frecuencia = reloj (APB 80 MHz, RC_FAST 17,5 MHz o XTAL 40 MHz según `LEDC_CONF`) / (CLK_DIV en punto fijo 10.8 × 2^resolución); ciclo = DUTY (4 bits de fracción) / 2^resolución. Cada cambio efectivo manda PWM (tipo 4) al GPIO cuya `FUNCn_OUT_SEL_CFG` tiene la señal 45 + canal; cambiar de pin apaga el anterior. Sin fades, IRQ ni pulsos GPIO. |
| ADC | `hw/misc/esp32c3_saradc.c`: ADC1 de una lectura (`ONETIME_SAMPLE`, `SAR1DATA_STATUS`, `ADC1_DONE` en INT_RAW/ST/CLR y su IRQ). ANALOGICO con GPIO 0..4 fija el valor crudo de 12 bits del canal 0..4; otros GPIO se ignoran. Sin ADC2, DMA ni calibración: `analogRead` devuelve el valor inyectado. |
| I2C | `hw/i2c/esp32c3_i2c.c`: I2C0 en `0x60013000`, FIFO APB de 32 bytes por DATA, ocho comandos C3 (WRITE=1, STOP=2, READ=3, END=4, RSTART=6), `conf_upgate`, `fsm_rst`, NACK e IRQ COMPLETE/END/FIFO. END conserva la transferencia; RSTART vuelve a seleccionar el esclavo. El mismo `taller_i2c` del clásico ofrece el espejo del puente en bus 0 y emite cada escritura al STOP/RSTART. Sin modo esclavo, DMA, temporización eléctrica ni I2C por software. |
| RMT | `hw/misc/esp32c3_rmt.c`: dos TX en `0x60016000`, RAM desde `+0x400`, 48 palabras por bloque TX (canal 0 puede tomar ambos). `conf_update`, inicio, fin, umbral e IRQ; recarga ping-pong con contrapresión como en el clásico. Cada tren emite RMT (tipo 7) con su inicio, símbolos crudos y GPIO de las señales 51/52. Tick = (1 + DIV_NUM + DIV_A/DIV_B) × divisor del canal / reloj, usando APB 80 MHz, RC_FAST 17,5 MHz o XTAL 40 MHz; divisor de canal 0 significa 256. Sin RX, DMA, portadora ni modo continuo. |
| SPI | SPI1 de flash conservado. Modelo independiente `hw/ssi/esp32c3_spi2.c`: SPI2/GPSPI2 maestro en `0x60024000`, W0–W15 desde `+0x98`, `CMD.update/usr` autolimpiables y `MS_DLEN` de 18 bits. CPU y GDMA TX/RX, MISO FF; cada fin vacía MOSI hacia `taller.spi`, **bus 2** del puente. `DMA_INT_RAW` R/W, ENA, ST=RAW&ENA y CLR W1C; IRQ `ETS_SPI2_INTR_SOURCE`, fin normal bit 12 y fin de segmento bit 13. |

Las capacidades y señales se contrastaron con `soc_caps.h`, `adc_channel.h`
y `gpio_sig_map.h` del mismo core. Los registros I2C y RMT se contrastaron
con `soc/{i2c,rmt}_{reg,struct}.h` y `hal/{i2c,rmt}_ll.h` del C3, además
del [driver `i2c_master` de IDF 5.5](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_driver_i2c/i2c_master.c). El bus del puente no necesita conocer
los GPIO SDA/SCL: el controlador resuelve directamente la dirección I2C.

SPI se contrastó con `soc/spi_{reg,struct}.h`, `soc/gdma_reg.h` y
`hal/{spi,gdma}_ll.h` instalados en Arduino **3.3.12**, y con el
[HAL SPI C3 de IDF 5.5](https://github.com/espressif/esp-idf/blob/v5.5/components/hal/esp32c3/include/hal/spi_ll.h)
y el [driver spi_master](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_driver_spi/src/gpspi/spi_master.c).
Las señales de matriz son **FSPICLK=63** y **FSPID/MOSI=65**. Taller asigna
el bus de la pieza según sus pines SCK/MOSI: para una ST7735 en C3 debe
resultar **bus 2**, también al usar el objeto Arduino `SPI` (FSPI).
No se envían pines dentro de la trama tipo 3: sigue siendo `u8 bus` + MOSI.
CS/DC continúan llegando por GPIO; `taller_puente_spi_fin(2)` preserva el
orden entre transacciones y cambios de GPIO. El puente conserva su troceo
de hasta 4096 bytes MOSI por trama; los bloques largos no pierden bytes.

El modelo usa los tres canales del **GDMA existente**, seleccionando
periférico 0/SPI2 **y** START de la dirección correspondiente (OUT bit 21,
IN bit 22). Los descriptores de 12 bytes contienen size/length/owner/EOF,
puntero y siguiente; TX consume length, RX size y escribe length/EOF.
CHECK_OWNER y OUT_AUTO_WRBACK se respetan. Solo accede a SRAM
`0x3fc80000..0x3fcdffff`, con direcciones absolutas: la `dma_as` de
Espressif tiene como raíz el alias DRAM, que está en `0x3FC80000`, y QEMU
conserva ese desplazamiento. Cadenas inválidas o cíclicas terminan con error GDMA;
si MS_DLEN corta un descriptor TX, no se devuelve OWNER ni se genera
OUT_DONE/EOF antes de consumir todos sus bytes.
Los enlaces consumidos no se reutilizan sin otro START. Se corrigen stride
de IRQ C3 (0x10 por canal), traducción de sus cinco eventos IN/cuatro OUT
y pulsos STOP/reset. IN/OUT se unen con OR antes de la fuente compartida.
No se modifica el motor genérico GDMA ni la matriz IRQ: esta ya recalcula
el nivel al reasignar (E-039). RAW y ENA de SPI también recalculan el nivel,
incluido el fin que IDF escribe antes de arrancar su primera ISR.
Los eventos RAW de fin normal y fin de segmento se acumulan hasta CLR o
escritura explícita de RAW; iniciar otra operación no descarta una IRQ pendiente.

**Límites SPI2:** ejecución síncrona, datos de bytes completos por una línea,
sin temporización eléctrica, arbitraje entre canales, PSRAM, modo esclavo,
DMA circular/restart ni interpretación de buffers de configuración SCT.
Se conservan los registros de modo/reloj; no se simulan flancos de SCK,
inversión de matriz, IO_MUX ni dummy clocks en las tramas. El estado de fin
de segmento tiene RAW/ENA/ST/CLR propio; no implica soporte del protocolo SCT.

### Pruebas C3

- `c3_pwm` (`pruebas/c3_pwm_prueba/`): `ledcAttach(8, 50, 14)`, 5 % → 25 % →
  `ledcChangeFrequency(8, 100, 14)` → 0 %. El verificador exige esa secuencia
  en GPIO8, en orden y con un 0,5 % de margen, tramas PWM válidas para el C3 y
  la línea única `TALLER-OK c3_pwm`.
- `c3_analogico` (`pruebas/c3_analogico_prueba/`): `analogRead(0)` cada 20 ms;
  el verificador inyecta ANALOGICO(0, 2048) y ANALOGICO(0, 4095) y exige las
  lecturas 0 → 1900..2200 → 4000..4095 y `TALLER-OK c3_analogico`.
- `c3_i2c` (`pruebas/c3_i2c_prueba/`): Wire hardware, SDA4/SCL5 a 100 kHz.
  El lanzador registra 0x3C en bus 0 e inyecta 0xAB en el registro 0xD0;
  exige lectura con RSTART, escritura `00 AF`, ACK=0, NACK=2 en 0x3D
  ausente y la línea única `TALLER-OK c3_i2c`. También comprueba las dos
  escrituras del puente, `D0` y `00 AF`, en ese orden; los sondeos no emiten.
- `c3_rmt` (`pruebas/c3_rmt_prueba/`): `rmtWrite` de Arduino, GPIO8, 10 MHz,
  un bloque de 48 palabras. Envía 8 LED RGB=(17,34,51) y 30 LED
  RGB=(68,85,102), ambos en GRB con bits MSB primero. Los dos trenes
  fuerzan recarga por IRQ; el verificador exige colores y símbolos exactos,
  tick de 100 ns, fin cero, 193/721 símbolos y `TALLER-OK c3_rmt`.
- `c3_spi` (`pruebas/c3_spi_prueba/`): SCK4/MOSI6/MISO5, 20 MHz.
  `SPI.beginTransaction`/`transfer`/`writeBytes` emiten `abcd` por CPU.
  Después libera `SPI` y usa `spi_master` con `SPI_DMA_CH_AUTO` e ISR:
  patrón `00..FF` repetido 160 veces, **40960 bytes**, en dos transacciones
  de **32768+8192** (límite de MS_DLEN). Una lectura GDMA de cuatro bytes
  añade cuatro ceros MOSI y exige `C3-SPI-MISO=ffffffff` antes de la línea
  única `TALLER-OK c3_spi`. El verificador puro compara los **40968 bytes
  exactos en orden**, exige bus 2, HOLA v7 y tiempos no decrecientes.
  Arduino `writeBytes` usa bloques CPU de hasta 64 bytes incluso con un
  buffer grande; la llamada explícita a IDF garantiza que se ejercita GDMA.
- Las unitarias `c3_*_unitarias.py` prueban los verificadores y los lanzadores
  sin QEMU; `c3_i2c_modelo_unitarias.py` y `c3_rmt_modelo_unitarias.py`
  compilan los modelos reales mediante `c3_modelos.py`. I2C cubre NACK,
  RSTART, comandos mayores que FIFO, lotes END de 70 bytes, IRQ y reset;
  RMT cubre dos TX, relojes/divisores, recarga antes y después de limpiar
  el umbral, lectura diferida, parada y reset. El adaptador admite GCC/Clang
  o MSVC (`cl` desde una consola de desarrollo); sin compilador se omiten
  las pruebas C. LEDC y ADC1 reutilizan el mismo adaptador.
  `c3_spi_unitarias.py` ejecuta SPI2 real y emisor real del puente en bus 2:
  CPU de 64 bytes, dos bloques DMA, TX/RX en canales 0/1/2, cadenas,
  highpart, errores, ciclos, RAW/ENA/CLR, RX sin outlink y reset. También
  cubre descriptores y buffers fuera de SRAM o cruzando su límite, OWNER
  con y sin comprobación, canales ajenos/parados/en reset, RX/TX parciales,
  OUT_AUTO_WRBACK desactivado y fin de segmento pendiente entre operaciones.
  Prueba el lanzador por sockets y rechaza tiempos decrecientes con MOSI correcto.
  `c3_gdma_unitarias.py` compila la traducción
  real C3 y comprueba los nueve eventos y los registros de los tres canales.

Las imágenes se compilan como la de GPIO (abajo), cambiando el nombre del sketch.


`pruebas/c3_gpio_prueba/c3_gpio_prueba.ino` produce tres pulsos completos en
GPIO8, lee GPIO9 con INPUT_PULLUP, espera ENTRADA(9, 0), vuelve a leer y termina
con `TALLER-OK c3`. El verificador exige HOLA v7, niveles/salidas limitados a
22 bits, tres pulsos habilitados, tiempos no decrecientes y las líneas únicas
`C3-ANTES=1`, `C3-DESPUES=0`, `TALLER-OK c3`, en ese orden.

Imagen completa de **4 MiB**, bootloader en **0x0**, particiones en `0x8000`,
boot_app0 en `0xe000` y aplicación en `0x10000`. Se compiló con CLI 1.5.1 y
core 3.3.12, DIO/40 MHz y USB CDC desactivado para usar UART0:

```powershell
$cli = "$env:APPDATA\dev.taller.app\arduino-cli\1.5.1\arduino-cli.exe"
$config = "$env:APPDATA\dev.taller.app\arduino\arduino-cli.yaml"
& $cli --config-file $config board listall esp32c3
& $cli --config-file $config compile `
  --fqbn 'esp32:esp32:esp32c3:FlashMode=dio,FlashFreq=40,CDCOnBoot=default' `
  --build-path "$PWD\pruebas\c3_gpio_prueba\build" pruebas/c3_gpio_prueba
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación C3' }
Copy-Item pruebas/c3_gpio_prueba/build/c3_gpio_prueba.ino.merged.bin pruebas/c3_gpio_prueba.flash.bin
```

SHA-256 de la imagen incluida:
`71a31f166b7f8aff0027f8cf6080a7c72bae5ad6181bd21103ddca4df75150b2`.

Las nuevas imágenes fusionadas, también de **4 MiB**, se compilaron con
la misma CLI, core y FQBN:

| Imagen | SHA-256 |
|---|---|
| `c3_i2c_prueba.flash.bin` | `9770b2a1d1cc3c6c1e09771065b9fafde067160cab69d406360a4161426ea557` |
| `c3_rmt_prueba.flash.bin` | `249ca0475eb198017d8f8f6541b2d33dc334cd3c804431c6ec46dcf4553c9bfd` |
| `c3_spi_prueba.flash.bin` | `c985edee5b21f4f7f8777070ea820a3abfbf59e833481481047252242dcbacef` |

El lanzador usa una copia temporal de la flash. No carga la eFuse ECO3 del
clásico: el modelo C3 crea su eFuse en RAM con revisión **v0.3** y versión
de bloque **1.3** si no recibe fichero. Desactiva los watchdog de TIMG con
`timer.esp32c3.timg.wdt_disable=true` y conserva `-icount shift=2,align=off,sleep=on`.
La instalación debe incluir `share/esp32c3-rom.bin` junto al resto de ROM de QEMU.

```sh
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
python3 pruebas/qemu_prueba.py /ruta/qemu-system-riscv32 c3_gpio pruebas/c3_gpio_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-riscv32 c3_i2c pruebas/c3_i2c_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-riscv32 c3_rmt pruebas/c3_rmt_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-riscv32 c3_spi pruebas/c3_spi_prueba.flash.bin
bash pruebas/todas.sh /ruta/qemu-system-xtensa /ruta/qemu-system-riscv32
```

`todas.sh` conserva las once llamadas clásicas y ejecuta los seis modos C3 (`c3_gpio`, `c3_pwm`,
`c3_analogico`, `c3_i2c`, `c3_rmt` y `c3_spi`) y los tres S3 (`s3_gpio`, `s3_pwm` y `s3_analogico`).
Sin segundo argumento, busca `qemu-system-riscv32` junto al Xtensa; también
admite `QEMU_C3=/ruta/binario`. La omisión opcional de MicroPython sigue igual.
Las unitarias ejecutan el GPIO C3 real aislado, sus trenes/reset, el parser y
los receptores reales de ambas máquinas, además de comprobar el verificador.
En el paso I2C/RMT se compilaron los dos sketches y se ejecutaron **73 unitarias**:
sin fallos, ocho omitidas por requerir GCC/Clang. Los cuatro modelos C3
(LEDC, ADC1, I2C y RMT) se compilaron y ejecutaron aislados con MSVC C11;
también se compilaron y ejecutaron el GPIO y el parser/receptores reales,
incluido el despacho I2C del C3. `bash -n pruebas/todas.sh` pasó. WSL devolvió
`E_ACCESSDENIED` al intentar ejecutar las pruebas; se usó el compilador nativo.
**Pendientes compilación/enlace de QEMU y ejecución de `todas.sh` en WSL con
los binarios recompilados.** Las pruebas aisladas no certifican arranque,
registro QOM ni comportamiento temporal integrado; tampoco se ejecutó aquí
MicroPython. En el paso SPI2 se compilaron el sketch y su imagen de 4 MiB
con la CLI de Taller 1.5.1 y el FQBN DIO/40 MHz/CDC desactivado de arriba.
En la continuación del WIP se ejecutaron **101 unitarias en WSL, sin fallos
ni omisiones**, incluidos los modelos reales y las regresiones del SPI clásico.
Con MSVC pasan las mismas 101, con 11 omisiones de pruebas que requieren GCC/Clang.
WSL sí permitió esta vez compilar aisladamente `esp32c3_spi2.c`,
`esp32c3_gdma.c` y `esp32c3.c` con las cabeceras y opciones reales del árbol
base y `-Werror`; también pasó `bash -n pruebas/todas.sh`.
Se recompiló el sketch con el FQBN indicado: imagen idéntica a la incluida,
componentes/offsets y SHA-256 comprobados, checksum y hash de aplicación válidos
según esptool. La revisión independiente detectó dos fallos, corregidos con
regresiones que fallaron antes del cambio: OUT_DONE prematuro con TX parcial
y pérdida de SEG_DONE pendiente al iniciar una operación normal.
En QEMU real, la prueba `c3_spi` destapó un tercer fallo: el modelo restaba
`0x3FC80000` antes de leer la `dma_as`, pero sus direcciones son absolutas
(el alias DRAM está colocado en `0x3FC80000` y QEMU conserva ese
desplazamiento), así que todo descriptor salía ilegible. Corregido el modelo
y el doble de las unitarias; `todas.sh` pasa sus 21 modos. **v0.12.0**,
protocolo **v7**.

## ESP32-S3: GPIO, LEDC, ADC1 y puente (fase 6d)

Usar **qemu-system-xtensa -machine esp32s3**. La máquina de las capas
(`hw/xtensa/esp32s3.c`) sustituye el GPIO mínimo de Espressif y conecta el
mismo chardev `taller-puente`: HOLA, GPIO, PWM, DISPARO y TIEMPO **v7**.
ENTRADA, ENTRADA_ARMADA y ANALOGICO llegan a su receptor propio, sin cambiar
el protocolo: las máscaras GPIO ya son de 64 bits. Se conservan los demás
dispositivos de la máquina base `esp-develop-9.2.2-20260417`.

### Modelos y límites S3

El GPIO tiene **49 índices (0..48), de los que 45 son pines físicos**:
**GPIO0..21 y GPIO26..48**. GPIO22..25 y los bits superiores se ignoran,
también en entradas externas, tablas de registros y ambos pines de un tren.
Los registros, relativos a `DR_REG_GPIO_BASE = 0x60004000`, se contrastaron
con `soc/gpio_reg.h` del core Arduino ESP32 **3.3.12**:

| Registro | Banco bajo | Banco alto (bit 0 = GPIO32) |
|---|---|---|
| OUT / W1TS / W1TC | `0x04 / 0x08 / 0x0c` | `0x10 / 0x14 / 0x18` |
| ENABLE / W1TS / W1TC | `0x20 / 0x24 / 0x28` | `0x2c / 0x30 / 0x34` |
| IN, solo lectura | `0x3c` | `0x40` |

`GPIO_PINn_REG`, en `0x74 + 4·n`, conserva el valor escrito; PAD_DRIVER
(bit 2) activa drenador abierto. IN devuelve OUT en salida push-pull, el
nivel externo en entrada y su AND en salida de drenador abierto. Las
entradas externas empiezan altas: no se simulan resistencias ni
INPUT_PULLDOWN. STRAP (`0x38`) conserva la propiedad `strap_mode`, por
defecto **4**, arranque flash. IO_MUX, IRQ GPIO, filtros e inversión quedan
pendientes.

`FUNCn_OUT_SEL_CFG`, en `0x554 + 4·n`, conserva el registro completo y usa
**9 bits** de selección. Al reset selecciona software (**256**). Los
periféricos resuelven su primera ruta mediante `esp32s3_gpio_pin_de_senal`.
Los trenes reutilizan `taller_armado.h`, ahora con capacidad hasta GPIO48:
ocho ranuras, 200 pasos, reloj virtual, DISPARO, rearme persistente y
cancelación de la reproducción al reset, como en C3. Cada modelo mantiene
su límite propio (clásico 40, C3 22, S3 con su hueco). El constructor Python
`trama_entrada_armada(..., pines=49)` permite pines altos del S3; por defecto
sigue validando 40 índices para las pruebas del clásico.

| Pieza | Estado en S3 |
|---|---|
| LEDC | Reutiliza `hw/misc/esp32c3_ledc.c`, parametrizado a **8 canales LS y 4 timers**, con señales **73..80** (`LEDC_LS_SIG_OUT0..7` de `gpio_sig_map.h`). Base `0x60019000`; registros de canal, timer y reloj coinciden con C3. Frecuencia y ciclo usan el mismo cálculo, relojes y tramas PWM; cambiar la ruta apaga el pin anterior. El C3 conserva 6 canales y señales 45..50. Sin fades, IRQ ni pulsos GPIO individuales. |
| ADC1 | Modelo propio `hw/misc/esp32s3_saradc.c`: **10 canales, GPIO1..10**, según `adc_channel.h`. `adc_ll.h` usa **SENS**, base **`0x60008800`**, no `APB_SARADC.ONETIME_SAMPLE` del C3. `SAR_MEAS1_CTRL2` (`0x0c`) selecciona un pad en bits 30..19; START (17) completa la conversión y publica DONE (16) y dato (15..0), ambos de solo lectura. Bajar START limpia DONE; mantenerlo alto no repite conversión. `SARADC_MEAS_STATUS` permanece libre. ANALOGICO inyecta el crudo, limitado a 4095; las entradas sobreviven al reset. `SAR_MEAS2_CTRL2` (`0x30`) termina con cero para permitir la autocalibración de arranque de ADC2. Sin canales ADC2 externos, DMA, ULP, latencia, IRQ ni calibración analógica. |
| UART, TIMG, SYSTIMER, matriz IRQ, flash y GDMA | Modelos de Espressif conservados. El revisor confirmó previamente arranque Arduino por UART0 con flash DIO desde 0x0 y watchdog TIMG desactivado. |
| I2C / RMT / SPI de usuario | Pendientes en S3. RMT sigue como dispositivo sin implementar de la máquina base; el receptor S3 ignora las tramas I2C. SPI1 sigue sirviendo la flash. |

### Pruebas S3

- `s3_gpio`: tres pulsos completos en **GPIO2**, lectura **GPIO4** con
  INPUT_PULLUP y ENTRADA(4, 0). Exige `S3-ANTES=1`, `S3-DESPUES=0` y
  `TALLER-OK s3_gpio`, líneas únicas y ordenadas, HOLA v7 y máscaras válidas.
- `s3_pwm`: **GPIO4**, 50 Hz al 5 % → 25 % → 100 Hz → apagado. Exige las
  tramas PWM en orden, margen del 0,5 % y `TALLER-OK s3_pwm` único.
- `s3_analogico`: `analogRead(1)`; inyecta ANALOGICO(1, 2048) y luego 4095.
  Exige lecturas 0 → 1900..2200 → 4000..4095 y `TALLER-OK s3_analogico`.

Los verificadores puros y lanzadores se comparten con C3, parametrizados por
chip, GPIO y máscara válida. Cada ejecución usa una **copia privada** de la
flash de **4 MiB**, sin eFuse ECO3 del clásico, UART0, `-icount
shift=2,align=off,sleep=on` y
`-global driver=timer.esp32s3.timg,property=wdt_disable,value=true`.
La instalación de QEMU debe incluir la ROM `esp32s3_rev0_rom.bin`.

Los tres sketches e imágenes están en `pruebas/s3_*_prueba/` y
`pruebas/s3_*_prueba.flash.bin`. Compilados con CLI de Taller **1.5.1** y core
**3.3.12**; bootloader en **0x0**, particiones en **0x8000**, boot_app0 en
**0xe000** y aplicación en **0x10000**:

```powershell
$cli = "$env:APPDATA\dev.taller.app\arduino-cli\1.5.1\arduino-cli.exe"
$config = "$env:APPDATA\dev.taller.app\arduino\arduino-cli.yaml"
$fqbn = 'esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default'
foreach ($nombre in 's3_gpio_prueba', 's3_pwm_prueba', 's3_analogico_prueba') {
  $compilacion = Join-Path $PWD ".trabajo\$nombre"
  & $cli --config-file $config compile --fqbn $fqbn `
    --build-path $compilacion "pruebas/$nombre"
  if ($LASTEXITCODE -ne 0) { throw "Falló $nombre" }
  Copy-Item "$compilacion\$nombre.ino.merged.bin" "pruebas/$nombre.flash.bin"
}
```

SHA-256 de las imágenes incluidas:

| Imagen | SHA-256 |
|---|---|
| `s3_gpio_prueba.flash.bin` | `cf1c71264997a19975b0810ad1b51c367e4be777d9b400786b2306061b1f5407` |
| `s3_pwm_prueba.flash.bin` | `8351ca35f7e3fe6bdab0d9878a6ae352795dc6d90e70bcfdf63bf4ae8107a1db` |
| `s3_analogico_prueba.flash.bin` | `ec1337eca9dc628588fe472069f00900fc3779d0eaba87ab5e0a5e9eb3b14cd8` |

```sh
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa s3_gpio pruebas/s3_gpio_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa s3_pwm pruebas/s3_pwm_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa s3_analogico pruebas/s3_analogico_prueba.flash.bin
bash pruebas/todas.sh /ruta/qemu-system-xtensa /ruta/qemu-system-riscv32
```

El bloqueo de `s3_analogico` sin ninguna línea `S3-A=` se explica por el
constructor [`adc_hw_calibration()` de IDF 5.5.5](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_adc/adc_common.c):
calibra **ambas unidades y sus cuatro atenuaciones antes de `setup()`**.
Sin eFuses de calibración, `read_cal_channel()` pulsa START sin pads y espera
DONE también en ADC2 (`SENS+0x30`), que antes solo almacenaba escrituras.
Ahora completa con cero (GND interno); ADC1 conserva las entradas del puente.
En una lectura normal, `adc_oneshot_ll_start()` espera `MEAS_STATUS=0` en
`SENS+0x40` y `adc_oneshot_hal_convert()` espera DONE en `SENS+0x0c`.
El arbitraje APB afecta a ADC2; no hace falta simular ADC digital ni modificar
RTC_CNTL para resolver esta espera. La regresión C reproduce calibraciones
repetidas de ambas unidades, protección DATA/DONE, independencia y reset.

Tras recompilar QEMU con las capas actualizadas, esta traza opcional vuelca
todos los accesos SENS a stderr (L/E, dirección absoluta, tamaño y valor):

```sh
TALLER_ADC_TRAZA=1 python3 pruebas/qemu_prueba.py "$HOME/qemu-trabajo/instalar-x86_64-linux-gnu/qemu/bin/qemu-system-xtensa" s3_analogico pruebas/s3_analogico_prueba.flash.bin 2> /tmp/s3-adc.traza
```

Buscar `60008830`: tras START (bit 17), la lectura debe tener DONE (bit 16)
y dato cero; después deben verse conversiones ADC1 en `6000880c`.
El verificador exige lecturas UART `S3-A=0`, `S3-A=2048`, `S3-A=4095` y
`TALLER-OK s3_analogico`; si pasan, muestra `OK s3_analogico: GPIO1 0 → 2048 → 4095`.
Sin la variable, o con valor distinto de `1`, la traza permanece apagada.
El lanzador hereda stderr cuando está activa para evitar llenar su tubería
interna y bloquear QEMU durante la calibración.

Las unitarias compilan y ejecutan los modelos C reales aislados de QOM:
ambos bancos, GPIO48, huecos, selección de 9 bits, drenador abierto, trenes,
ocho canales LEDC/cuatro timers y diez canales ADC1, además de reset.
Prueban el receptor real S3 con el parser fragmentado y los tres lanzadores
mediante sockets, incluida fragmentación UART, inyecciones y flash privada.
También pasan las unitarias previas. Se compilaron aisladamente ocho unidades
C con cabeceras reales de QEMU (máquinas S3/C3, GPIO S3/C3/clásico, LEDC
compartido, ADC1 S3 y puente). **Pendientes enlace de QEMU y ejecución real
de `todas.sh` con los binarios recompilados**; las pruebas aisladas no
certifican registro QOM, arranque ni temporización integrada.
La investigación opcional de MicroPython S3 no se realizó en esta fase.

## Protocolo del puente (v7)

Con `-chardev socket,id=taller-puente,host=127.0.0.1,port=<p>,server=on,wait=on`, QEMU envía tramas
little-endian con una cabecera de 12 bytes: `u8 tipo, u8 version=7, u16 longitud_carga, u64 t_ns`.

| tipo | carga |
|---|---|
| 1 HOLA | `u32 version_protocolo` (= 7) |
| 2 GPIO | `u64 niveles, u64 salidas` (bit n = GPIO n) |
| 3 SPI  | `u8 bus` (clásico: 2 = HSPI, 3 = VSPI; C3: **2 = SPI2/FSPI**) + bytes MOSI de una transacción, troceados si superan 4096 bytes |
| 4 PWM | `u8 gpio, u32 frecuencia_mHz, u32 ciclo_ppm` (9 bytes, sin relleno) |
| 5 I2C_ESCRITURA | `u8 bus, u8 direccion, bytes…`: escritura completa, incluido el puntero; hasta 4094 bytes de datos por trama |
| 6 DISPARO | `u8 pin_disparo`; `t_ns` es el instante virtual del flanco que inicia el tren |
| 7 RMT | `u8 canal, u8 gpio, u32 tick_ps, u32 símbolos…`: símbolos crudos LE; `t_ns` es el inicio de TX; se envía al terminar |
| 8 TIEMPO | sin carga; `t_ns` es la hora virtual. Latido cada 10 ms virtuales desde HOLA (v7) |
| 16 ENTRADA (hacia QEMU) | `u8 pin, u8 nivel`: nivel externo de un pin de entrada (botones, sensores) |
| 17 ANALOGICO (hacia QEMU) | `u8 gpio, u16 valor` (3 bytes, sin relleno): muestra cruda ADC1, saturada a 4095 |
| 18 I2C_DISPOSITIVO (hacia QEMU) | `u8 bus, u8 direccion, u8 activo`: alta o baja de una dirección |
| 19 I2C_REGISTROS (hacia QEMU) | `u8 bus, u8 direccion, u8 registro_inicial, bytes…`: hasta 240 bytes para el espejo |
| 20 ENTRADA_ARMADA (hacia QEMU) | `u8 pin_disparo, u8 flanco, u32 minimo_ns, u8 pin_salida, u16 n`, seguidos de `n × (u32 espera_ns, u8 nivel)`; `9 + 5·n` bytes, sin relleno |

Sin ese chardev, QEMU se comporta como el de Espressif.

La versión 7 añade TIEMPO: un latido cada 10 ms virtuales con la hora del ESP32, para que el
receptor la sepa aunque el firmware no toque ningún periférico (el arranque o un programa que solo
usa el Serial). Taller lo usa como reloj de los escenarios. Un receptor que no conozca el tipo 8 lo salta.
La versión 6 añade RMT a la versión 5, conserva las cargas anteriores y acepta las
tramas entrantes anteriores. HOLA y todas las tramas salientes anuncian 6.
La versión 5 añade ENTRADA_ARMADA y DISPARO a la versión 4, conservando sus cargas.
La versión 4 añade I2C a la versión 3, conservando las cargas anteriores. La versión 3
añade PWM y ANALOGICO a la versión 2. La frecuencia se expresa en milihercios
(50 Hz = 50000 mHz) y el ciclo en partes por millón (5 % = 50000 ppm;
0 apaga la salida, 1000000 representa el 100 %). SPI pendiente se envía antes
de GPIO o PWM para conservar el orden de los cambios.
La frecuencia se satura a `4294967295 mHz` si supera el rango del `u32`.

ENTRADA_ARMADA instala un tren persistente: `flanco=0` para bajada y `flanco=1`
para subida, después de permanecer al menos `minimo_ns` en el nivel contrario.
Se vigila por pin el nivel **conducido por el ESP32**, `enable ? out : 1`, y su
instante de último cambio en `QEMU_CLOCK_VIRTUAL`. En entrada cuenta como alto
por el pull-up del módulo: los pulsos externos del propio sensor no lo redisparan.
Todos los pines empiezan altos y su antigüedad inicial se cuenta desde `t=0`.
Cambiar OUT sin habilitar la salida no cambia ese nivel ni reinicia su antigüedad.

Hay ocho ranuras, una por `pin_disparo`; un armado con el mismo pin sustituye al
anterior. `n=0` desarma y `n<=200`; GPIO de disparo/salida `0..39` y niveles `0/1`.
Longitud, flanco, pines y **todos** los pasos se validan antes de cambiar el estado;
una trama mal formada se ignora entera. Una novena ranura se ignora. Una ranura
desarmada que aún suena no se puede reutilizar para otro pin hasta que termine.
El búfer entrante admite los 1021 bytes de una trama máxima; las tramas que lo
exceden se drenan completas para conservar la siguiente cabecera.

Al detectar el flanco se emite un único DISPARO y se programa el primer paso.
Cada paso espera `espera_ns` desde el anterior (el primero desde el flanco) y
fija el nivel externo de `pin_salida` por la misma ruta que ENTRADA. Las esperas
cero se aplican en el mismo instante. Los plazos son absolutos y acumulativos,
con temporizadores del reloj virtual: una ejecución tardía no desplaza los pasos
siguientes. Mientras suena se ignoran nuevos disparos de ese pin. Un rearme o
desarmado durante el tren afecta al siguiente disparo; el tren en curso termina
con su copia de los pasos y del pin originales. Al reset del chip se cancelan
los trenes en curso, se conservan los armados y se reinicia la antigüedad de los
niveles altos en ese instante, sin generar DISPARO. SPI pendiente se vacía antes
de DISPARO. El `t_ns` de ENTRADA_ARMADA se ignora, como en las demás entradas.

I2C usa buses 0 y 1 en el clásico y solo bus 0 en el C3, con hasta ocho direcciones activas de 7 bits por bus
(1..127; no se admite general call). Sin alta, la dirección responde NACK.
Cada alta nueva crea un espejo de 256 bytes a cero y puntero a cero; repetir el alta
conserva el estado, y la baja lo borra. Una novena alta y buses/direcciones no válidos
se ignoran. Con puente se omite el TMP105 de demostración de Espressif en 0x48.
En cada escritura, el primer byte fija el puntero y los siguientes actualizan el espejo
con autoincremento; cada lectura devuelve `espejo[puntero++]`. El puntero vuelve a cero
tras 255. I2C_REGISTROS actualiza el espejo sin mover ese puntero; una dirección sin alta
se ignora. El estado externo se conserva durante resets del controlador.
I2C_ESCRITURA se envía al STOP o al START siguiente; un sondeo sin bytes no genera trama.
El búfer de escritura tiene 4096 bytes y se trocea en cargas de hasta 4096 bytes
(bus + dirección + hasta 4094 datos). Por encima de 4096 bytes se drenan bloques durante
la transferencia, sin perder bytes ni interpretar otro puntero. Cada fragmento repite
bus/dirección; v4 no incluye identificador ni marca de último fragmento. SPI pendiente
se envía antes de I2C_ESCRITURA. Las tramas entrantes ignoran `t_ns`.

ANALOGICO ignora `t_ns` y conserva el valor hasta otra trama, incluso durante un reset.
Los ocho canales ADC1 empiezan en cero. La correspondencia GPIO → canal es
36→0, 37→1, 38→2, 39→3, 32→4, 33→5, 34→6 y 35→7; otros GPIO se ignoran.
SENS (`0x3ff48800`, alias APB `0x60008800`, 0x400 bytes) completa la conversión RTC
al flanco ascendente de `MEAS1_START_SAR`, con DONE y DATA de solo lectura.
Si se seleccionan varios canales, usa el bit más bajo; sin canal válido devuelve cero.
Los demás registros guardan lo escrito y se borran al reset del dominio RTC.
No se emulan ADC2, DMA, latencia, atenuación ni calibración: el valor inyectado ya es crudo de 12 bits.

LEDC conserva los ocho timers y los dieciséis canales HS/LS del ESP32 clásico.
Se usa APB de 80 MHz, o REF_TICK de 1 MHz cuando `TICK_SEL=0`; no se simulan fades
ni la fuente RC de 8 MHz. Los cambios LS se recalculan también antes de `PARA_UP`,
sin repetir muestras idénticas. Si una señal aparece en varios GPIO, se usa el primero;
al cambiar de pin se apaga el anterior.

## SPI con DMA (v0.7.0)

SPI2 (HSPI) y SPI3 (VSPI) leen MOSI y escriben MISO mediante cadenas de
descriptores `lldesc_t` en SRAM interna (`0x3ffae000..0x3fffffff`). Esto permite
usar `machine.SPI` de MicroPython con el driver `spi_master` de ESP-IDF 5.x,
que inicializa el bus con `SPI_DMA_CH_AUTO`. MicroPython `SPI(2)` corresponde
a **VSPI/SPI3**; `SPI(1)` corresponde a HSPI/SPI2.

Registros añadidos al modelo (desplazamientos desde cada controlador):

| Registros | Semántica |
|---|---|
| `DMA_CONF` `0x100` | Conserva configuración; `in_rst`/`out_rst` reinician su motor y estado de descriptores. Los resets comunes FIFO/AHB no desmontan enlaces ya armados. Los bits de reset conservan el valor escrito hasta que el driver los baja. |
| `DMA_OUT_LINK` `0x104`, `DMA_IN_LINK` `0x108` | Dirección inicial de 20 bits, reconstruida como `0x3ff00000 \| addr`; `start`/`restart` arman el motor y se autolimpian; `stop` lo detiene. |
| `DMA_STATUS` `0x10c` | `rx_en`/`tx_en` indican motores armados. |
| `DMA_INT_ENA/RAW/ST/CLR` `0x110..0x11c` | RAW retenido, ST = RAW & ENA; escribir uno en CLR limpia el bit correspondiente. |
| Descriptores/EOF `0x120..0x144` | Descriptor actual, siguiente, búfer y último EOF procesados. |
| `DMA_RSTATUS/TSTATUS` `0x148/0x14c` | Estado de reposo, cero; no se simula la evolución de las FIFO. |
| `SLAVE` `0x38` | Estado real R/W de `trans_done` (bit 4) y habilitación `trans_inten` (bit 9). También permite que IDF provoque la IRQ escribiendo ambos bits. |

`DPORT_SPI_DMA_CHAN_SEL_REG` (`0x3ff005a8`) conserva seis bits: SPI1 en [1:0],
SPI2 en [3:2] y SPI3 en [5:4]. Para SPI2/3, 0 desactiva DMA, 1/2 seleccionan
canal y 3 es reservado. La IRQ SPI conserva las fuentes 30/31; las IRQ DMA
son independientes y llegan a las fuentes 53/54.

Orden del driver contrastado con `spi_ll.h` del core 3.3.12 y
[ESP-IDF 5.5](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_driver_spi/src/gpspi/spi_master.c):

1. Al inicializar el bus, selecciona canal DMA en DPORT y configura SPI/IRQ.
2. Al preparar cada transacción, limpia `SLAVE.trans_done`, comprueba `CMD=0`
   y configura `MOSI_DLEN`, `MISO_DLEN`, comando, dirección y temporización.
3. Para RX, prepara descriptores, pulsa `in_rst` y los resets de FIFO/AHB,
   escribe la dirección de `DMA_IN_LINK` y pone `start`.
4. Para TX, prepara descriptores, pulsa `out_rst` y los resets de FIFO/AHB,
   escribe la dirección de `DMA_OUT_LINK` y pone `start`. En TX sin RX, IDF
   puede arrancar antes un inlink NULL como workaround del silicio; sin fase
   MISO, el modelo no accede a ese descriptor.
5. Habilita MOSI/MISO después de preparar DMA y escribe `CMD.usr`.
6. El modelo completa la transferencia y fija `trans_done`; el driver lee
   ese bit por polling o recibe la IRQ SPI. El driver también lo escribe a
   uno al inicializar, para poder despertar la ISR cuando llega trabajo.

Cada descriptor contiene tres palabras little-endian: control, dirección de
búfer y dirección completa del siguiente descriptor. TX usa `length[23:12]`,
sin superar MOSI_DLEN ni continuar después de EOF. RX usa `size[11:0]`, escribe
la longitud recibida y pone EOF en el último descriptor utilizado. Ambos
devuelven `owner` a CPU. Los campos que `lldesc_t` llama `offset`/`sosf`
son reservados para SPI en el [TRM, sección 2.3.2](https://documentation.espressif.com/esp32_technical_reference_manual_en.pdf)
y se conservan sin interpretarlos. TX marca `out_done`, `out_eof` y
`out_total_eof`; RX marca `in_done` e `in_suc_eof`. Una cadena inválida, un
owner CPU, una dirección ajena a SRAM o un fallo de memoria marca error de
descriptor; RX sin capacidad suficiente marca `inlink_dscr_empty`.

MOSI sigue pasando por SSI, `taller_puente_spi_byte` y `taller_puente_spi_fin`.
El núcleo recibe las mismas cargas v6; no hay mensaje DMA nuevo. Un framebuffer
de 40 960 bytes llega completo, troceado por el puente en bloques de hasta
4096 bytes. MicroPython además divide su escritura en transacciones de hasta
4092 bytes. El verificador concatena todas y exige cada byte en orden.
MISO procede de SSI; sin esclavo que responda devuelve `0xff`.
Las transacciones sin DMA siguen usando W0..W15, incluido Arduino/ST7735.
Se corrige además el índice del bucle PIO: la comparación de límites usa la
posición, permitiendo recibir todos los bytes incluso con MOSI `0xff`.

### Prueba MicroPython

El firmware externo no se incorpora a Git. Descarga
[ESP32_GENERIC-20260824-v1.29.0.bin](https://micropython.org/resources/firmware/ESP32_GENERIC-20260824-v1.29.0.bin),
SHA-256 `e67ad6015a0a504c1fec9aa9bbf589d0432ed28e62546f4f8dd8a147f8bd95f6`.
`preparar_micropython.py` comprueba ese hash y coloca el firmware en **0x1000**
de una flash de **4 MiB rellena de 0xff**, no en el offset cero:

```sh
python3 pruebas/preparar_micropython.py /ruta/ESP32_GENERIC-20260824-v1.29.0.bin
# Sin argumento, el preparador descarga la misma URL y verifica el hash.
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa spi_dma pruebas/micropython.flash.bin
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
bash pruebas/todas.sh /ruta/qemu-system-xtensa
```

`spi_dma_micropython.py` crea `SPI(2, baudrate=20_000_000, sck=Pin(18), mosi=Pin(23))`,
escribe `b"abcd"`, escribe `bytes(range(256)) * 160`, lee cuatro bytes y exige
`ffffffff`; termina con `TALLER-OK spi_dma`. El modo `spi_dma` introduce ese
programa por REPL raw paste, con control de ventana y plazo de espera. Exige
HOLA v7 y **40968 bytes exactos de VSPI**: los cuatro iniciales, el framebuffer
y los cuatro ceros MOSI que genera `read(4)`. Rechaza pérdidas, sobrantes,
cambios de orden y excepciones aunque aparezca la marca de éxito.
Cada ejecución usa una copia temporal de la flash para proteger la imagen
original de las escrituras VFS de MicroPython.

`todas.sh` añade la prueba cuando existe `pruebas/micropython.flash.bin`.
También admite `MICROPYTHON_FLASH=/ruta/flash.bin`; si se indica esa variable
y falta la imagen, falla. Sin imagen ni variable, anuncia la omisión y sigue
ejecutando todos los modos anteriores.

Las unitarias compilan en WSL la cabecera pura, el modelo SPI real aislado
de los servicios QEMU y el emisor SPI real del puente. Cubren una única
transacción de 40 960 bytes con 11 descriptores, TX/RX en HSPI/VSPI, SSI,
IRQ separadas, start/stop/reset/canal, cadenas inválidas, límites de longitud
y recepción sin sobreescribir centinelas. También prueban el verificador y
el protocolo raw paste por sockets.

**Límites:** ejecución síncrona, sin duración de reloj SPI/DMA, contención
simultánea de canales, modo continuo/circular, DMA SPI0/1 ni PSRAM. Los modos
burst se conservan como configuración sin efecto temporal. `OUT_AUTO_WRBACK`
no condiciona la devolución de owner en este modelo. Los errores detienen
el recorrido y completan la transacción con sus banderas; no se reproducen
bloqueos de silicio. La lógica acota cada recorrido a 32768 descriptores y
solo accede a SRAM interna; un descriptor guest no puede ejecutar MMIO.
Las pruebas aisladas no sustituyen ejecutar `spi_dma` y los modos anteriores
con el QEMU recompilado.

## Matriz de interrupciones

`hw/xtensa/esp32_intc.c` es una copia modificada del de Espressif. La matriz guarda el nivel de
cada fuente, y la línea de cada interrupción de CPU es el OR de todas las fuentes asignadas a ella;
se recalcula cuando cambia un nivel y cuando se reasigna una fuente. El original ponía la línea al
nivel de la última fuente que cambiaba y no miraba los niveles al reasignar. Pero ESP-IDF activa y
desactiva las interrupciones cambiando su asignación (`esp_intr_enable`/`disable`), y el driver
SPI deja `trans_done` alto antes de activarla para que su ISR entre en cuanto se active: con el
original, `machine.SPI` de MicroPython se quedaba esperando para siempre.

## GPIO de drenador abierto

`GPIO_PINn_REG` (0x88 + 4·n) se guarda entero y su bit 2 (`PAD_DRIVER`) marca el pin como
drenador abierto. En un pin de salida así, `GPIO_IN` da el AND cableado de lo que conduce el
ESP32 y lo que llega de fuera: un 1 suelta la línea y manda el exterior. El módulo `dht` de
MicroPython deja el pin del DHT22 en drenador abierto y lee así la respuesta del sensor; sin
esto leía siempre su propio 1. Los trenes armados vigilan `enable ? out : 1`, que en drenador
abierto sigue siendo correcto (un 1 es una línea suelta).

## Prueba PWM

`pruebas/pwm_prueba/pwm_prueba.ino` usa Arduino ESP32 core 3.x. Su imagen completa
de flash está en `pruebas/pwm_prueba.flash.bin` (generada con el core 3.3.12 y
`esp32:esp32:esp32:FlashMode=dio,FlashFreq=40`; QEMU necesita arranque DIO).
El modo `pwm` admite HOLA v3/v4/v5/v6, exige la marca UART y una muestra en GPIO13 de
50 Hz ±1 % con ciclo del 5 % ±0,3 %:

```sh
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa pwm pruebas/pwm_prueba.flash.bin
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
```

## Prueba analógica

`pruebas/analogico_prueba/analogico_prueba.ino` lee GPIO34 cada 20 ms con resolución de 12 bits.
`pruebas/analogico_prueba.flash.bin` contiene la flash completa, compilada con Arduino ESP32
core 3.3.12 y `esp32:esp32:esp32:FlashMode=dio,FlashFreq=40`.
El modo `analogico` admite HOLA v3/v4/v5/v6 y exige líneas UART completas: `A=0` inicial,
1900..2200 tras ANALOGICO(34, 2048) y 4000..4095 tras ANALOGICO(34, 4095).
En este core, `analogRead` devuelve el resultado crudo; con 12 bits no cambia la resolución
ni aplica la calibración de `analogReadMilliVolts`. Se conservan los márgenes solicitados.
El HAL de ADC1 RTC consulta también `SENS_SAR_SLAVE_ADDR1.MEAS_STATUS` (cero tras reset),
pero no consulta APB_SARADC/SYSCON para completar esta conversión; esas rutas corresponden
al controlador digital o a ADC2.

```sh
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa analogico pruebas/analogico_prueba.flash.bin
```

## Prueba I2C

`pruebas/i2c_prueba/i2c_prueba.ino` usa `Wire` del core Arduino ESP32 3.x,
bus 0, SDA 21 y SCL 22. Espera un segundo antes de escanear 0x08..0x77.
El modo `i2c` espera HOLA v4/v5/v6, registra 0x3C y precarga 0xD0/0xD1 con 0x60/0x61.
Exige únicamente `ENCONTRADO 0x3C`, `LEIDO=60,61`, `TALLER-OK i2c` y
una trama I2C_ESCRITURA con bus 0, dirección 0x3C y bytes `00 AF`.
La imagen completa `pruebas/i2c_prueba.flash.bin` se genera con core 3.3.12 y
`esp32:esp32:esp32:FlashMode=dio`:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32:FlashMode=dio --build-path pruebas/i2c_prueba/build pruebas/i2c_prueba
cp pruebas/i2c_prueba/build/i2c_prueba.ino.merged.bin pruebas/i2c_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa i2c pruebas/i2c_prueba.flash.bin
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
```

## Prueba de trenes armados

`pruebas/armado_prueba/armado_prueba.ino` configura TRIG bajo, anuncia
`ARMADO-LISTO` y espera un segundo al inicio para que el script arme ambos trenes
tras HOLA v5/v6. Esperar esa marca evita contar como disparo la propia habilitación
inicial de GPIO5: pasa de entrada (conducido alto) a salida baja.

- A, GPIO4: subida tras al menos 800 µs en bajo. Pasos relativos: 30 µs→0,
  80 µs→1, 80 µs→0, 50 µs→1, 70 µs→0, 50 µs→1. El sketch mantiene el pin bajo
  1100 µs y lo suelta con `INPUT_PULLUP`, dos veces. El script exige `DHT=80,80,50,70`
  y `DHT2=80,80,50,70`, cada ancho con tolerancia ±3 µs. Un bajo de 300 µs debe
  dejar la línea alta durante los siguientes 500 µs: `CORTO=SIN_RESPUESTA`.
- B, TRIG=GPIO5 y ECHO=GPIO18: bajada tras al menos 8 µs en alto (un `delayMicroseconds(10)` dura ~9,9 µs). Pasos:
  450 µs→1 y 1166 µs→0. Se inicia ECHO bajo por ENTRADA. Un pulso TRIG de 10 µs
  se mide con `pulseIn(18, HIGH, 30000)` y debe dar `ECO=1163..1169`; uno de 5 µs
  debe dar `ECO_CORTO=0`.

El modo `armado` exige además `TALLER-OK armado`, exactamente dos DISPARO del pin 4
y uno del pin 5, sin otros DISPARO ni tiempos decrecientes. `pruebas/todas.sh`
incluye este modo; todos los modos previos admiten HOLA 6.

Para A se mide con `esp_cpu_get_cycle_count()` a 240 MHz (comprobado con
`static_assert`), lectura directa del registro GPIO y las interrupciones locales
suspendidas durante la captura. Un ciclo equivale a 4,17 ns; ambos flancos usan
el mismo bucle y se redondea la diferencia a µs. Así se reduce el coste por sondeo
y se evita la cuantización de `micros()`. En B se conserva `pulseIn`, que en el
core 3.3.12 también usa el contador de ciclos. La tolerancia de ±3 µs se comprueba
con el QEMU compilado y `-icount`; las pruebas unitarias no certifican esa medida.

La imagen **completa de flash**, de 4 MiB, es `pruebas/armado_prueba.flash.bin`:
salida `armado_prueba.ino.merged.bin` del core 3.3.12, FQBN
`esp32:esp32:esp32:FlashMode=dio`. Para regenerarla desde PowerShell:

```powershell
$cli = "$env:APPDATA\dev.taller.app\arduino-cli\1.5.1\arduino-cli.exe"
$config = "$env:APPDATA\dev.taller.app\arduino\arduino-cli.yaml"
$compilacion = Join-Path (Get-Location) '.trabajo\armado-compilacion'
& $cli --config-file $config compile --fqbn esp32:esp32:esp32:FlashMode=dio `
  --build-path $compilacion pruebas/armado_prueba
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación de armado_prueba' }
Copy-Item "$compilacion\armado_prueba.ino.merged.bin" pruebas/armado_prueba.flash.bin
Remove-Item -LiteralPath $compilacion -Recurse -Force
```

```sh
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa armado pruebas/armado_prueba.flash.bin
bash pruebas/todas.sh /ruta/qemu-system-xtensa
```

`armado_unitarias.py` compila y ejecuta la lógica C pura de `taller_armado.h`,
incluyendo rearme/desarmado durante un tren, mínimo inclusivo, 200 pasos, esperas
cero, GPIO39, ocupación de ranuras y reset. Compila también el receptor real
aislado de QOM/chardev para comprobar el descarte de tramas excesivas y fragmentadas.
Requiere `cc`, `gcc` o `clang` (en Windows sin compilador omite esos dos casos;
ejecutar la suite en WSL). Las demás pruebas del verificador usan solo Python.

## RMT TX y prueba de NeoPixel

`esp32.rmt` sustituye el hueco del SoC en `0x3ff56000` (alias APB `0x60016000`,
0x1000 bytes). Emula ocho canales TX y 512 palabras compartidas, en bloques de
64 por canal. RAM directa en `+0x800`, con `APB_CONF.fifo_mask=1`; con cero,
`CHnDATA` permite acceso FIFO. `CHnCONF0/1` conservan divisor, tamaño, propietario,
modo continuo y nivel/habilitación de reposo. `MEM_RD_RST` reinicia el lector y
el contador de umbral; `APB_MEM_RST`, los punteros FIFO. Son pulsos de control,
igual que `TX_START` y `REF_CNT_RST`. `CHnSTATUS` refleja lector, activo y memoria
agotada; no emula estados RX. Con tamaño cero o RAM propiedad de RX, TX no arranca
y marca el bit de error del canal. Los tamaños que exceden la RAM se acotan al
final de las 512 palabras. Un reset cancela timers, descarta trenes incompletos,
borra RAM y restaura registros e IRQ.

Cada palabra contiene `duration0:15, level0:1, duration1:15, level1:1`.
Una duración cero termina TX; si es la segunda, se consume primero la duración
inicial. Se conservan las palabras crudas, incluido el marcador de fin leído.
Con `mem_tx_wrap_en`, el lector vuelve al principio de los bloques del canal.
Sin wrap, agotar los bloques termina el tren. `tx_conti_mode` repite desde el
principio al encontrar fin o agotar memoria sin wrap, sin `tx_end` entre vueltas;
cada vuelta completa se publica como un tren. Un bucle vacío termina para evitar
un temporizador infinito sin avance de tiempo.

`tx_thr_event` aparece cada `tx_lim` símbolos consumidos (`tx_lim=0` lo desactiva),
independientemente de su máscara de interrupción. El contador se reinicia en cada
aviso, aunque RAW siga pendiente; un marcador de duración inicial cero no cuenta.
`tx_end` aparece al finalizar el último intervalo. RAW queda retenido;
ST = RAW & ENA, CLR limpia los bits escritos a uno. La IRQ de nivel llega a
`ETS_RMT_INTR_SOURCE` (47). APB se fija a 80 MHz y REF a 1 MHz, según
`ref_always_on`; divisor cero representa 256. El tick se fija al comenzar TX.
Los plazos usan `QEMU_CLOCK_VIRTUAL`, acumulando picosegundos y redondeando el
plazo total hacia arriba a ns. Así se conservan los ticks APB de 12,5 ns.
Solo se consulta la siguiente palabra cuando empieza su intervalo: los símbolos
ya enviados quedan copiados, aunque el firmware sobrescriba la RAM en la ISR.

El HAL Arduino del core 3.3.12 llama al driver RMT TX de ESP-IDF 5.5 con un
codificador de copia. `Adafruit NeoPixel` 1.15.5 pide **un bloque**, resolución
**10 MHz** (APB / 8) y usa símbolos alto/bajo de 4/8 ticks para cero y 8/4 para uno.
El driver configura wrap y umbral de **32 palabras**, llena inicialmente 64 y
alterna la recarga de ambas mitades en la ISR. Al completar la codificación
introduce un símbolo de duración cero y deshabilita la IRQ de umbral. El modelo
separa cargar y completar un símbolo para respetar ese orden y sus tiempos.

`MEM_OWNER` (bit 5 de `CHnCONF1`) **no** bloquea la transmisión: el driver deja el
valor de reinicio (1) en los canales de TX y el chip real transmite igual. Ese bit
solo decide quién escribe la RAM al recibir. Para diagnosticar el driver,
`TALLER_RMT_TRAZA=1` vuelca a la salida de error cada acceso a los registros del
RMT (no a su RAM) y cada cambio de la IRQ.

**Contrapresión en el umbral (v0.6.1).** Con la IRQ `tx_thr_event` activada, el canal
se para al marcarla y sigue cuando la ISR la limpia en `INT_CLR` (o la desactiva en
`INT_ENA`). En el chip, la ISR entra en pocos µs y le sobran los 40 µs de la otra
mitad. Pero en QEMU, con el firmware en `waiti` y `-icount sleep=on`, el reloj virtual
corre con el real, y la ISR podía llegar tarde: el modelo leía la mitad sin rellenar,
con colores distintos en cada ejecución. Los símbolos no cambian; el tren dura unos
µs más (8 LED: 282-290 µs, hasta ~330 µs con el equipo cargado).

La trama RMT llega **al terminar**, con `t_ns` del **inicio** del tren y el primer
GPIO conectado a `RMT_SIG_OUT0_IDX + canal` (87 + canal), capturado al iniciar;
si no hay ninguno, 255. SPI pendiente se vacía antes. Por ello, el `t_ns` de RMT
puede ser anterior al de otras tramas emitidas durante TX: no hay monotonía
global entre tipos. RMT no emite pulsos GPIO individuales; el nivel de reposo
se conserva en los registros y el consumidor recibe los niveles en los símbolos.
Sin puente ni pin conectado, timers, umbral, fin e IRQ funcionan igualmente.

Cada carga admite **16382 símbolos** (6 + 4·16382 = 65534 bytes). Un tren mayor
se trocea en tramas consecutivas con idénticos canal, GPIO, tick y `t_ns`, sin
partir palabras ni intercalar otras tramas. No hay índice ni marca de último
fragmento: el receptor debe reunir esos fragmentos antes de tratar el final de
la transmisión como reset WS2812. Las transmisiones incompletas se guardan en
memoria del anfitrión hasta terminar; un firmware con wrap infinito y sin fin
puede hacer crecer ese búfer. RX, DMA, portadora y cambios de reloj durante TX
quedan fuera del modelo.

`pruebas/rmt_prueba/rmt_prueba.ino` envía en GPIO13 los ocho colores
rojo, verde, azul, blanco, apagado, (1,2,3), (128,64,32), (255,255,0), y después
30 LED con RGB `(7·i, 255−5·i, 3·i)`, `i=0..29`. El verificador usa `tick_ps`,
umbral alto >0,55 µs, orden GRB y MSB primero; exige ambos trenes completos,
GPIO13, HOLA 6 y `TALLER-OK rmt`.

`T_SHOW` mide `micros()` alrededor del primer `show()` y debe estar entre
**200 y 2000 µs**, inclusivos. Se prepara el mismo canal con `rmtInit` antes de
medir para excluir su `delay(1)`. El tren nominal de esta versión de NeoPixel
dura **230,4 µs** (192 × 1,2 µs); ISR, codificación y despertar de la tarea añaden
coste. La medida real y la integración se comprueban después de compilar QEMU.
El modo `humo` permite comprobar también el firmware sin puente.

La imagen completa de 4 MiB `pruebas/rmt_prueba.flash.bin` está compilada con
CLI 1.5.1, core 3.3.12, NeoPixel 1.15.5 y `esp32:esp32:esp32:FlashMode=dio`:

```powershell
$cli = "$env:APPDATA\dev.taller.app\arduino-cli\1.5.1\arduino-cli.exe"
$config = "$env:APPDATA\dev.taller.app\arduino\arduino-cli.yaml"
$compilacion = Join-Path (Get-Location) '.trabajo\rmt-compilacion'
& $cli --config-file $config lib install 'Adafruit NeoPixel@1.15.5'
& $cli --config-file $config compile --fqbn esp32:esp32:esp32:FlashMode=dio `
  --build-path $compilacion pruebas/rmt_prueba
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación de rmt_prueba' }
Copy-Item "$compilacion\rmt_prueba.ino.merged.bin" pruebas/rmt_prueba.flash.bin
Remove-Item -LiteralPath $compilacion -Recurse -Force
```

```sh
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa rmt pruebas/rmt_prueba.flash.bin
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa humo pruebas/rmt_prueba.flash.bin 'TALLER-OK rmt'
bash pruebas/todas.sh /ruta/qemu-system-xtensa
```

`rmt_logica.c` prueba la misma cabecera que usa el dispositivo: ping-pong de
192/720 símbolos, varios bloques, ocho canales, umbral periódico y no divisor
del bloque, wrap, duración cero en ambas mitades, bucle, límites, reset, relojes,
memoria directa/FIFO e interrupciones. `rmt_unitarias.py` compila ese C en WSL,
prueba colores y fragmentos del verificador y compila el emisor real aislado
de chardev para verificar LE y troceado de 20000 símbolos. Estas pruebas no
sustituyen la compilación de QEMU ni certifican la latencia real de su IRQ.
