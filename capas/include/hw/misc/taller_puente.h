/*
 * Puente de Taller: envía a un programa externo, por un chardev, lo que el firmware
 * hace con los pines (GPIO), el SPI y el PWM, con marca de tiempo virtual.
 *
 * Protocolo v3 (little-endian; v2 + PWM y ANALOGICO). Cabecera de 12 bytes:
 *   u8 tipo, u8 version (=3), u16 longitud_carga, u64 t_ns (QEMU_CLOCK_VIRTUAL)
 * Tipos (QEMU -> programa externo):
 *   1 HOLA  carga: u32 version_protocolo (=3)
 *   2 GPIO  carga: u64 niveles, u64 salidas   (bit n = GPIO n)
 *   3 SPI   carga: u8 bus (2 = HSPI, 3 = VSPI) + bytes MOSI de una transacción (máx. 4096)
 *   4 PWM   carga: u8 gpio, u32 frecuencia_mHz, u32 ciclo_ppm (9 bytes, sin relleno)
 *           frecuencia en milihercios; ciclo de 0 a 1000000 ppm; 0 apaga la salida
 * Tipos (programa externo -> QEMU; t_ns se ignora):
 *  16 ENTRADA carga: u8 pin, u8 nivel  (nivel externo de un pin de entrada; t_ns se ignora)
 *             disponible desde v2
 *  17 ANALOGICO carga: u8 gpio, u16 valor (3 bytes, sin relleno; disponible desde v3)
 *              valor ADC1 de 12 bits, saturado a 4095; GPIO ajenos a ADC1 se ignoran
 *              GPIO 36,37,38,39,32,33,34,35 -> canales ADC1 0,1,2,3,4,5,6,7
 *
 * Se activa solo si existe un chardev con id "taller-puente".
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "chardev/char.h"

#define TYPE_TALLER_SPI "taller.spi"
#define TALLER_PUENTE_CHARDEV "taller-puente"
#define TALLER_PUENTE_VERSION 3

enum {
    TALLER_TRAMA_HOLA = 1,
    TALLER_TRAMA_GPIO = 2,
    TALLER_TRAMA_SPI = 3,
    TALLER_TRAMA_PWM = 4,
    TALLER_TRAMA_ENTRADA = 16,
    TALLER_TRAMA_ANALOGICO = 17,
};

void taller_puente_iniciar(Chardev *chr);
bool taller_puente_activo(void);
/* Vuelca antes cualquier SPI pendiente, para que el orden sea el real. */
void taller_puente_gpio(uint64_t niveles, uint64_t salidas);
void taller_puente_pwm(uint8_t gpio, uint32_t frecuencia_mhz, uint32_t ciclo_ppm);
void taller_puente_spi_byte(uint8_t bus, uint8_t mosi);
void taller_puente_spi_fin(uint8_t bus);
