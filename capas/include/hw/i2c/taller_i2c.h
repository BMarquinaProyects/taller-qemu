/*
 * Espejo I2C de Taller: dos buses, ocho direcciones de 7 bits por bus.
 * Cada dirección conserva 256 registros y un puntero con vuelta de 255 a 0.
 * Los datos externos sobreviven a los resets del controlador ESP32.
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "hw/i2c/i2c.h"

#define TYPE_TALLER_I2C "taller.i2c"
#define TALLER_I2C_BUSES 2
#define TALLER_I2C_DIRECCIONES 8
#define TALLER_I2C_REGISTROS 256
#define TALLER_I2C_ESCRITURA_MAX 4096

/* Preparar antes de habilitar la recepción del puente. */
void taller_i2c_preparar(I2CBus *bus, uint8_t numero);
/* Alta idempotente; baja borra el espejo. Bus/dirección no válidos se ignoran.
 * General call (dirección 0) no se admite. Una novena alta se ignora. */
void taller_i2c_dispositivo(uint8_t bus, uint8_t direccion, bool activo);
/* Solo actualiza direcciones dadas de alta; no mueve el puntero del firmware. */
void taller_i2c_registros(uint8_t bus, uint8_t direccion, uint8_t registro,
                         const uint8_t *datos, size_t largo);
