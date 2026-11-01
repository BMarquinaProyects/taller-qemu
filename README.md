# taller-qemu

QEMU de Espressif para ESP32 con periféricos extra (GPIO real, ADC1, puente de GPIO/SPI/PWM/analógico/I2C) que usa
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

## Protocolo del puente (v4)

Con `-chardev socket,id=taller-puente,host=127.0.0.1,port=<p>,server=on,wait=on`, QEMU envía tramas
little-endian con una cabecera de 12 bytes: `u8 tipo, u8 version=4, u16 longitud_carga, u64 t_ns`.

| tipo | carga |
|---|---|
| 1 HOLA | `u32 version_protocolo` (= 4) |
| 2 GPIO | `u64 niveles, u64 salidas` (bit n = GPIO n) |
| 3 SPI  | `u8 bus` (2 = HSPI, 3 = VSPI) + bytes MOSI de una transacción |
| 4 PWM | `u8 gpio, u32 frecuencia_mHz, u32 ciclo_ppm` (9 bytes, sin relleno) |
| 5 I2C_ESCRITURA | `u8 bus, u8 direccion, bytes…`: escritura completa, incluido el puntero; hasta 4094 bytes de datos por trama |
| 16 ENTRADA (hacia QEMU) | `u8 pin, u8 nivel`: nivel externo de un pin de entrada (botones, sensores) |
| 17 ANALOGICO (hacia QEMU) | `u8 gpio, u16 valor` (3 bytes, sin relleno): muestra cruda ADC1, saturada a 4095 |
| 18 I2C_DISPOSITIVO (hacia QEMU) | `u8 bus, u8 direccion, u8 activo`: alta o baja de una dirección |
| 19 I2C_REGISTROS (hacia QEMU) | `u8 bus, u8 direccion, u8 registro_inicial, bytes…`: hasta 240 bytes para el espejo |

Sin ese chardev, QEMU se comporta como el de Espressif.

La versión 4 añade I2C a la versión 3, conservando las cargas anteriores. La versión 3
añade PWM y ANALOGICO a la versión 2. La frecuencia se expresa en milihercios
(50 Hz = 50000 mHz) y el ciclo en partes por millón (5 % = 50000 ppm;
0 apaga la salida, 1000000 representa el 100 %). SPI pendiente se envía antes
de GPIO o PWM para conservar el orden de los cambios.
La frecuencia se satura a `4294967295 mHz` si supera el rango del `u32`.

I2C usa buses 0 y 1, con hasta ocho direcciones activas de 7 bits por bus
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

## Prueba PWM

`pruebas/pwm_prueba/pwm_prueba.ino` usa Arduino ESP32 core 3.x. Su imagen completa
de flash está en `pruebas/pwm_prueba.flash.bin` (generada con el core 3.3.12 y
`esp32:esp32:esp32:FlashMode=dio,FlashFreq=40`; QEMU necesita arranque DIO).
El modo `pwm` admite HOLA v3/v4, exige la marca UART y una muestra en GPIO13 de
50 Hz ±1 % con ciclo del 5 % ±0,3 %:

```sh
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa pwm pruebas/pwm_prueba.flash.bin
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
```

## Prueba analógica

`pruebas/analogico_prueba/analogico_prueba.ino` lee GPIO34 cada 20 ms con resolución de 12 bits.
`pruebas/analogico_prueba.flash.bin` contiene la flash completa, compilada con Arduino ESP32
core 3.3.12 y `esp32:esp32:esp32:FlashMode=dio,FlashFreq=40`.
El modo `analogico` admite HOLA v3/v4 y exige líneas UART completas: `A=0` inicial,
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
El modo `i2c` espera HOLA v4, registra 0x3C y precarga 0xD0/0xD1 con 0x60/0x61.
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
