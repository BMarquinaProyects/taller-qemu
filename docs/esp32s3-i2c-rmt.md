# ESP32-S3: contraste I2C0 y RMT TX con Arduino 3.3.12

Cabeceras leídas de la instalación de Taller:
`%APPDATA%/dev.taller.app/arduino/data/packages/esp32/tools/`:

- `esp32{c3,s3}-libs/3.3.12/include/soc/esp32{c3,s3}/register/soc/{i2c,rmt}_{reg,struct}.h`.
- `esp32{c3,s3}-libs/3.3.12/include/hal/esp32{c3,s3}/include/hal/{i2c,rmt}_ll.h`.
- `esp32s3-libs/3.3.12/include/soc/esp32s3/include/soc/{gpio_sig_map,interrupts}.h`.
- `esp32{c3,s3}-libs/3.3.12/ld/esp32{c3,s3}.peripherals.ld`, para las bases
  de registros y `RMTMEM` que usa el controlador al escribir símbolos.

El HAL prevalece sobre los comentarios generados: `i2c_struct.h` S3 describe
opcodes antiguos en un comentario, pero `i2c_ll.h` define RSTART=6, WRITE=1,
READ=3, STOP=2 y END=4, exactamente como C3.

## I2C: mismo mapa y mismos bits

Se compararon los 314 campos numéricos comunes `_S`/`_V` de `i2c_reg.h`:
ninguna diferencia de posición o anchura. La estructura S3 usa tipos de unión
separados y otros nombres C, sin cambiar el mapa. No necesita parámetro de chip.
La máquina S3 instancia `ESP32C3I2CState` y `TYPE_ESP32C3_I2C`.

| Offset C3 = S3 | Registro | Tratamiento compartido |
|---|---|---|
| 0x00 | SCL_LOW_PERIOD | Almacena configuración; sin temporización eléctrica. |
| 0x04 | CTR | MASTER=4, START=5, FSM_RST=10, CONF_UPGATE=11; pulsos autocancelados. |
| 0x08 | SR | NACK=0, BUS_BUSY=4, RX_COUNT=13..8, TX_COUNT=23..18; solo lectura. |
| 0x0c | TO | Almacena configuración; sin timeout eléctrico. |
| 0x10 | SLAVE_ADDR | Almacena configuración; sin modo esclavo. |
| 0x14 | FIFO_ST | Punteros RX_R/RX_W/TX_R/TX_W en 0/5/10/15; solo lectura. |
| 0x18 | FIFO_CONF | Umbrales RX/TX en 0/5; resets FIFO en 12/13. |
| 0x1c | DATA | Acceso APB byte a byte a FIFO de 32 bytes. |
| 0x20 | INT_RAW | Condiciones FIFO y eventos END/COMPLETE/NACK; solo lectura. |
| 0x24 | INT_CLR | W1C; las condiciones FIFO de nivel se recalculan. |
| 0x28 | INT_ENA | Máscara de 18 bits. |
| 0x2c | INT_STATUS | RAW & ENA; solo lectura. |
| 0x30 | SDA_HOLD | Almacena configuración. |
| 0x34 | SDA_SAMPLE | Almacena configuración. |
| 0x38 | SCL_HIGH_PERIOD | Almacena configuración. |
| 0x3c | Reservado | Lectura cero; escritura ignorada. |
| 0x40 | SCL_START_HOLD | Almacena configuración. |
| 0x44 | SCL_RSTART_SETUP | Almacena configuración. |
| 0x48 | SCL_STOP_HOLD | Almacena configuración. |
| 0x4c | SCL_STOP_SETUP | Almacena configuración. |
| 0x50 | FILTER_CFG | Almacena configuración; sin filtrado eléctrico. |
| 0x54 | CLK_CONF | Almacena configuración; sin temporización eléctrica. |
| 0x58 | COMD0 | Comando de 14 bits; DONE=31. |
| 0x5c | COMD1 | Igual que COMD0. |
| 0x60 | COMD2 | Igual que COMD0. |
| 0x64 | COMD3 | Igual que COMD0. |
| 0x68 | COMD4 | Igual que COMD0. |
| 0x6c | COMD5 | Igual que COMD0. |
| 0x70 | COMD6 | Igual que COMD0. |
| 0x74 | COMD7 | Igual que COMD0. |
| 0x78 | SCL_ST_TIME_OUT | Almacena configuración. |
| 0x7c | SCL_MAIN_ST_TIME_OUT | Almacena configuración. |
| 0x80 | SCL_SP_CONF | Autocancela SCL_RST_SLV_EN; sin pulsos eléctricos. |
| 0x84 | SCL_STRETCH_CONF | Almacena configuración; sin stretch eléctrico. |
| 0x88..0xf4 | Reservados | Lectura cero; escritura ignorada. |
| 0xf8 | DATE | Solo lectura; el modelo conserva el reset cero del C3. |
| 0xfc | Reservado | Lectura cero; escritura ignorada. |
| 0x100..0x17c | TXFIFO_MEM[32] | No implementado; Arduino usa DATA según `i2c_ll_write_txfifo`. |
| 0x180..0x1fc | RXFIFO_MEM[32] | No implementado; Arduino lee DATA. |

Diferencias de nombres: `timeout` → `to`, `fifo_data` → `data`,
`command[8]` → `comd[8]`, `command0_done` → `command_done`. Los macros C3
`TIME_OUT_REG`, `SCL_ST_TO_REG`, `SCL_MAIN_ST_TO_REG` pasan a `TIME_OUT_VALUE`,
`SCL_ST_TO_I2C`, `SCL_MAIN_ST_TO_I2C` en S3, con las mismas anchuras y posiciones.
S3 declara explícitamente las ventanas FIFO de RAM y `I2C1`; esta fase conecta
solo I2C0. C3 y S3 tienen el maestro usado aquí en `0x60013000`.

`taller_i2c_preparar(bus, 0)` se ejecuta antes de HOLA. El receptor S3 admite
I2C_DISPOSITIVO(18) e I2C_REGISTROS(19) de bus 0; ignora otros buses y cargas
cortas. El espejo compartido emite I2C_ESCRITURA(5), incluidos puntero y datos,
al STOP/RSTART. END conserva la transferencia. El puntero y registros externos
sobreviven al reset del maestro. Esto ofrece el transporte de Wire para SSD1306
y registros BME280; los valores físicos y efectos del dispositivo los aporta Taller.

## RMT: mapa parametrizado, motor TX común

La propiedad QOM `s3=true` selecciona `mapa_s3` antes de realizar el dispositivo.
Se conserva `esp32c3_rmt.c`, su motor por temporizadores y su transporte v7.
El callback `pin_de_senal` evita dependencias con el GPIO de otra arquitectura.
`realize` aplica el reset del mapa seleccionado después de fijar propiedades.

| Grupo | C3 | S3 |
|---|---|---|
| Base | 0x60016000 | 0x60016000 |
| Canales | TX0..1, RX2..3 | TX0..3, RX4..7 |
| DATA de todos los canales | 0x00..0x0c | 0x00..0x1c |
| CONF0 TX | 0x10..0x14 | 0x20..0x2c |
| CONF0/1 RX | 0x18..0x24 | 0x30..0x4c |
| STATUS TX | 0x28..0x2c | 0x50..0x5c |
| STATUS RX | 0x30..0x34 | 0x60..0x6c |
| RAW / ST / ENA / CLR | 0x38 / 0x3c / 0x40 / 0x44 | 0x70 / 0x74 / 0x78 / 0x7c |
| Portadora TX | 0x48..0x4c | 0x80..0x8c |
| Portadora RX | 0x50..0x54 | 0x90..0x9c |
| TX_LIM | 0x58..0x5c | 0xa0..0xac |
| RX_LIM | 0x60..0x64 | 0xb0..0xbc |
| SYS_CONF / TX_SIM / REF_CNT_RST | 0x68 / 0x6c / 0x70 | 0xc0 / 0xc4 / 0xc8 |
| DATE | 0xcc, valor 0x02006231 | 0xcc, valor 0x02101181 |
| RAM directa | 0x400..0x6fc, 192 palabras | 0x800..0xdfc, 384 palabras |
| Palabras por bloque | 48 | 48 |
| MEM_SIZE TX, desde bit 16 | 3 bits | 4 bits |
| STATUS: dirección RAM | bits 8..0 | bits 9..0 |
| STATUS: puntero escritura APB | bits 20..12 | bits 20..11 |
| STATUS: estado TX | desde bit 9 | desde bit 22 |
| STATUS: MEM_EMPTY / APB_MEM_WR_ERR | bits 22 / 23 | bits 25 / 26 |
| Señales GPIO TX | 51..52 | 81..84 |

TX_START=0, MEM_RD_RST=1, APB_MEM_RST=2, WRAP=4, STOP=7, DIV=15..8,
AFIFO_RST=23 y CONF_UPDATE=24 coinciden. También coinciden las IRQ TX:
END=canal, ERROR=4+canal, THRES=8+canal. RX S3 tiene su grupo de IRQ
en bits 16..27; escribir sus registros no arranca TX. RX, DMA, portadora,
sincronización TX_SIM y modo continuo no se emulan.

Cada TX puede tomar bloques TX posteriores, limitados al final de TX3 en S3
o TX1 en C3. Nunca toma bloques RX. Umbral y wrap permiten recargar las mitades
de RAM sin precargar símbolos futuros. La contrapresión espera la ISR y admite
INT_CLR antes o después del encoder. STOP y reset cancelan temporizadores y
descartan trenes incompletos.

Las bases RAM son diferentes: `RMTMEM=0x60016400` en C3 y `0x60016800` en S3,
según sus scripts de enlace. Las pruebas del modelo usan estas direcciones
independientes del mapa implementado; usar +0x400 en S3 consume ceros en TX.

SYS_CONF conserva fuentes APB 80 MHz, RC_FAST 17,5 MHz y XTAL 40 MHz:
`tick_ps = 10^12 × (DIV_canal o 256) × (1 + NUM + A/B) / fuente_Hz`.
La trama RMT(7) contiene canal TX, GPIO resuelto, tick en picosegundos y todos
los símbolos crudos, incluido el fin. El puente conserva el instante inicial
al trocear. El modelo ofrece los símbolos para WS2812/NeoPixel; no genera sus
flancos como tramas GPIO individuales.

## Interrupciones y Windows (E-045)

Las fuentes S3 son RMT=40 e I2C_EXT0=42 según `interrupts.h` y la cabecera
de QEMU. `hw/xtensa/esp32s3_intc.c` de Espressif mantiene `irq_map[cpu][fuente]`
y busca el índice Xtensa de salida. No calcula `BIT(fuente)` ni usa máscaras
`long`: estas fuentes no sufren el truncamiento del C3 corregido en v0.12.1.
No se modifica esa matriz. Las máscaras GPIO del puente son de 64 bits y los
verificadores S3 aceptan GPIO48, rechazando el hueco 22..25 y los índices ≥49.

## Verificación y límites

Los sketches reproducen los patrones C3: Wire en SDA8/SCL9, ID=AB,
escritura `00 AF`, ACK=0/NACK=2; RMT en GPIO10, tick de 100 ns y trenes GRB de
8 y 30 LED, con 193/721 símbolos y fin explícito. Usan Wire y `rmtWrite`, sin
bibliotecas Adafruit. Las pruebas aisladas ejecutan el código real común con
ambos mapas y el parser/receptor real con tramas fragmentadas.

Las imágenes se compilan con CLI Taller 1.5.1 y el FQBN documentado en README;
se comprueban tamaño de 4 MiB, DIO, identificador S3 y componentes en
0x0/0x8000/0xe000/0x10000. La ejecución completa en QEMU Linux/Windows y con
SSD1306, BME280 y Adafruit_NeoPixel queda pendiente para el revisor. WSL no pudo
ejecutarse en esta sesión (`Wsl/Service/CreateInstance/E_ACCESSDENIED`); las
compilaciones y pruebas aisladas disponibles se ejecutan con MSVC nativo.
