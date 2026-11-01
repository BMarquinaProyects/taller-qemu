/*
 * Puente de Taller: envía a un programa externo, por un chardev, lo que el firmware
 * hace con los pines (GPIO), SPI, PWM e I2C, con marca de tiempo virtual.
 *
 * Protocolo v4 (little-endian; v3 + I2C). Cabecera de 12 bytes:
 *   u8 tipo, u8 version (=4), u16 longitud_carga, u64 t_ns (QEMU_CLOCK_VIRTUAL)
 * Tipos (QEMU -> programa externo):
 *   1 HOLA  carga: u32 version_protocolo (=4)
 *   2 GPIO  carga: u64 niveles, u64 salidas   (bit n = GPIO n)
 *   3 SPI   carga: u8 bus (2 = HSPI, 3 = VSPI) + bytes MOSI de una transacción (máx. 4096)
 *   4 PWM   carga: u8 gpio, u32 frecuencia_mHz, u32 ciclo_ppm (9 bytes, sin relleno)
 *           frecuencia en milihercios; ciclo de 0 a 1000000 ppm; 0 apaga la salida
 *   5 I2C_ESCRITURA carga: u8 bus, u8 direccion, bytes escritos (incluye puntero)
 *           al STOP o START siguiente; máx. 4094 bytes de datos por trama
 *           transacciones largas se trocean, repitiendo bus/dirección, sin cabecera extra
 * Tipos (programa externo -> QEMU; t_ns se ignora):
 *  16 ENTRADA carga: u8 pin, u8 nivel  (nivel externo de un pin de entrada; t_ns se ignora)
 *             disponible desde v2
 *  17 ANALOGICO carga: u8 gpio, u16 valor (3 bytes, sin relleno; disponible desde v3)
 *              valor ADC1 de 12 bits, saturado a 4095; GPIO ajenos a ADC1 se ignoran
 *              GPIO 36,37,38,39,32,33,34,35 -> canales ADC1 0,1,2,3,4,5,6,7
 *  18 I2C_DISPOSITIVO carga: u8 bus, u8 direccion, u8 activo (alta/baja; desde v4)
 *  19 I2C_REGISTROS carga: u8 bus, u8 direccion, u8 registro_inicial, bytes (máx. 240)
 *              bus 0/1, direcciones de 7 bits salvo general call; ocho altas por bus
 *              primer byte escrito fija puntero, siguientes actualizan espejo de 256
 *              registros; lectura devuelve espejo[puntero++], con vuelta a cero
 *              sin alta no hay ACK; REGISTROS no mueve el puntero del firmware
 *
 * Se activa solo si existe un chardev con id "taller-puente".
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "chardev/char.h"

#define TYPE_TALLER_SPI "taller.spi"
#define TALLER_PUENTE_CHARDEV "taller-puente"
#define TALLER_PUENTE_VERSION 4

enum {
    TALLER_TRAMA_HOLA = 1,
    TALLER_TRAMA_GPIO = 2,
    TALLER_TRAMA_SPI = 3,
    TALLER_TRAMA_PWM = 4,
    TALLER_TRAMA_I2C_ESCRITURA = 5,
    TALLER_TRAMA_ENTRADA = 16,
    TALLER_TRAMA_ANALOGICO = 17,
    TALLER_TRAMA_I2C_DISPOSITIVO = 18,
    TALLER_TRAMA_I2C_REGISTROS = 19,
};

void taller_puente_iniciar(Chardev *chr);
bool taller_puente_activo(void);
/* Vuelca antes cualquier SPI pendiente, para que el orden sea el real. */
void taller_puente_gpio(uint64_t niveles, uint64_t salidas);
void taller_puente_pwm(uint8_t gpio, uint32_t frecuencia_mhz, uint32_t ciclo_ppm);
void taller_puente_spi_byte(uint8_t bus, uint8_t mosi);
void taller_puente_spi_fin(uint8_t bus);
void taller_puente_i2c_escritura(uint8_t bus, uint8_t direccion,
                                  const uint8_t *datos, size_t largo);
