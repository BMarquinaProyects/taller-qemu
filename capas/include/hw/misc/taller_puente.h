/*
 * Puente de Taller: envía a un programa externo, por un chardev, lo que el firmware
 * hace con los pines (GPIO), SPI, PWM, I2C y RMT, con marca de tiempo virtual.
 *
 * Protocolo v7 (little-endian; v6 + TIEMPO). Cabecera de 12 bytes:
 *   u8 tipo, u8 version (=7), u16 longitud_carga, u64 t_ns (QEMU_CLOCK_VIRTUAL)
 * Tipos (QEMU -> programa externo):
 *   1 HOLA  carga: u32 version_protocolo (=7)
 *   2 GPIO  carga: u64 niveles, u64 salidas   (bit n = GPIO n)
 *   3 SPI   carga: u8 bus (2 = HSPI, 3 = VSPI) + bytes MOSI de una transacción (máx. 4096)
 *   4 PWM   carga: u8 gpio, u32 frecuencia_mHz, u32 ciclo_ppm (9 bytes, sin relleno)
 *           frecuencia en milihercios; ciclo de 0 a 1000000 ppm; 0 apaga la salida
 *   5 I2C_ESCRITURA carga: u8 bus, u8 direccion, bytes escritos (incluye puntero)
 *           al STOP o START siguiente; máx. 4094 bytes de datos por trama
 *           transacciones largas se trocean, repitiendo bus/dirección, sin cabecera extra
 *   6 DISPARO carga: u8 pin_disparo; t_ns = instante virtual del flanco (desde v5)
 *   7 RMT carga: u8 canal, u8 gpio, u32 tick_ps, u32 simbolos... (desde v6)
 *           símbolos crudos LE, incluido fin; gpio=255 si no está conectado.
 *           Se emite al terminar; t_ns = inicio del tren. Máx. 16382 símbolos
 *           por trama (65534 bytes). Trozos consecutivos repiten canal/gpio/tick
 *           y t_ns, sin indicador adicional de fin; no se parte un símbolo.
 *   8 TIEMPO sin carga; t_ns = reloj virtual (desde v7). Latido cada 10 ms
 *           virtuales desde HOLA: el receptor sabe la hora del ESP32 aunque
 *           el firmware no toque ningún periférico (arranque, Serial puro).
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
 *  20 ENTRADA_ARMADA (desde v5): u8 pin_disparo, u8 flanco (0=bajada, 1=subida),
 *              u32 minimo_ns, u8 pin_salida, u16 n, n*(u32 espera_ns, u8 nivel).
 *              Sin relleno: 9+5*n bytes; GPIO 0..39 (C3: 0..21), nivel 0/1, n<=200.
 *              n=0 desarma; máximo ocho ranuras, una por pin_disparo.
 *              Cada espera se cuenta desde el paso anterior (primera: flanco).
 *              Vigila enable ? out : 1; inicial alto desde t=0. El flanco exige
 *              antigüedad >= minimo_ns en el nivel opuesto. Armado persistente.
 *              Mientras suena no se redispara; rearmar/desarmar afecta al futuro,
 *              nunca a los pasos en curso. Una carga inválida se ignora entera.
 *
 * Se activa solo si existe un chardev con id "taller-puente".
 * Cada máquina recibe sus cargas mediante TallerPuenteReceptor. El C3 solo
 * admite ENTRADA y ENTRADA_ARMADA en esta fase; ADC/I2C siguen en el clásico.
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#pragma once

#include "chardev/char.h"

#define TYPE_TALLER_SPI "taller.spi"
#define TALLER_PUENTE_CHARDEV "taller-puente"
#define TALLER_PUENTE_VERSION 7
/* Cada cuánto se manda TIEMPO, en ns de QEMU_CLOCK_VIRTUAL. */
#define TALLER_PUENTE_LATIDO_NS (10 * 1000 * 1000)

enum {
    TALLER_TRAMA_HOLA = 1,
    TALLER_TRAMA_GPIO = 2,
    TALLER_TRAMA_SPI = 3,
    TALLER_TRAMA_PWM = 4,
    TALLER_TRAMA_I2C_ESCRITURA = 5,
    TALLER_TRAMA_DISPARO = 6,
    TALLER_TRAMA_RMT = 7,
    TALLER_TRAMA_TIEMPO = 8,
    TALLER_TRAMA_ENTRADA = 16,
    TALLER_TRAMA_ANALOGICO = 17,
    TALLER_TRAMA_I2C_DISPOSITIVO = 18,
    TALLER_TRAMA_I2C_REGISTROS = 19,
    TALLER_TRAMA_ENTRADA_ARMADA = 20,
};

/* Receptor propio de la máquina; recibe una carga completa, sin cabecera. */
typedef void (*TallerPuenteReceptor)(void *opaque, uint8_t tipo,
                                    const uint8_t *datos, size_t largo);
void taller_puente_iniciar(Chardev *chr, TallerPuenteReceptor receptor,
                           void *opaque);
bool taller_puente_activo(void);
/* Vuelca antes cualquier SPI pendiente, para que el orden sea el real. */
void taller_puente_gpio(uint64_t niveles, uint64_t salidas);
void taller_puente_pwm(uint8_t gpio, uint32_t frecuencia_mhz, uint32_t ciclo_ppm);
void taller_puente_disparo(uint8_t pin, int64_t t_ns);
void taller_puente_rmt(uint8_t canal, uint8_t gpio, uint32_t tick_ps,
                        const uint32_t *simbolos, size_t cantidad, int64_t t_ns);
void taller_puente_spi_byte(uint8_t bus, uint8_t mosi);
void taller_puente_spi_fin(uint8_t bus);
void taller_puente_i2c_escritura(uint8_t bus, uint8_t direccion,
                                  const uint8_t *datos, size_t largo);
