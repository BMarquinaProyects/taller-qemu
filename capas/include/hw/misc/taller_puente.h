/*
 * Puente de Taller: envía a un programa externo, por un chardev, lo que el firmware
 * hace con los pines (GPIO) y el SPI, con marca de tiempo virtual.
 *
 * Protocolo v2 (little-endian; v1 + tramas de entrada). Cabecera de 12 bytes:
 *   u8 tipo, u8 version, u16 longitud_carga, u64 t_ns (QEMU_CLOCK_VIRTUAL)
 * Tipos (QEMU -> programa externo):
 *   1 HOLA  carga: u32 version_protocolo (=2)
 *   2 GPIO  carga: u64 niveles, u64 salidas   (bit n = GPIO n)
 *   3 SPI   carga: u8 bus (2 = HSPI, 3 = VSPI) + bytes MOSI de una transacción (máx. 4096)
 * Tipos (programa externo -> QEMU), protocolo v2:
 *  16 ENTRADA carga: u8 pin, u8 nivel  (nivel externo de un pin de entrada; t_ns se ignora)
 *
 * Se activa solo si existe un chardev con id "taller-puente".
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "chardev/char.h"

#define TYPE_TALLER_SPI "taller.spi"
#define TALLER_PUENTE_CHARDEV "taller-puente"
#define TALLER_PUENTE_VERSION 2

enum {
    TALLER_TRAMA_HOLA = 1,
    TALLER_TRAMA_GPIO = 2,
    TALLER_TRAMA_SPI = 3,
    TALLER_TRAMA_ENTRADA = 16,
};

void taller_puente_iniciar(Chardev *chr);
bool taller_puente_activo(void);
/* Vuelca antes cualquier SPI pendiente, para que el orden sea el real. */
void taller_puente_gpio(uint64_t niveles, uint64_t salidas);
void taller_puente_spi_byte(uint8_t bus, uint8_t mosi);
void taller_puente_spi_fin(uint8_t bus);
