/* SPI2/FSPI S3: motor de registros compartido con C3. GPL-2.0-or-later. */
#pragma once
#include "hw/ssi/esp32c3_spi2.h"

#define TYPE_ESP32S3_SPI2 "esp32s3.spi2"
#define ESP32S3_SPI2_BUS ESP32C3_SPI2_BUS
typedef ESP32C3Spi2State ESP32S3Spi2State;
