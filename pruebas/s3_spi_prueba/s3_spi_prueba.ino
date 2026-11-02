// SPI2 S3: CPU con SPI Arduino y GDMA con spi_master de IDF 5.5.
// FQBN: esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default
#include <Arduino.h>
#include <SPI.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

static constexpr int SCK_SPI = 12;
static constexpr int MOSI_SPI = 11;
static constexpr int MISO_SPI = 13;

static bool dma_prueba() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = SCK_SPI;
  bus.mosi_io_num = MOSI_SPI;
  bus.miso_io_num = MISO_SPI;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 32768;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    return false;
  }
  spi_device_interface_config_t config = {};
  config.clock_speed_hz = 20000000;
  config.spics_io_num = -1;
  config.queue_size = 2;
  spi_device_handle_t dispositivo = nullptr;
  if (spi_bus_add_device(SPI2_HOST, &config, &dispositivo) != ESP_OK) {
    spi_bus_free(SPI2_HOST);
    return false;
  }
  uint8_t *datos = static_cast<uint8_t *>(heap_caps_malloc(32768, MALLOC_CAP_DMA));
  bool ok = datos != nullptr;
  if (ok) {
    for (unsigned i = 0; i < 32768; i++) {
      datos[i] = static_cast<uint8_t>(i);
    }
    // MS_DLEN tiene 18 bits: máximo 32768 bytes por transacción S3.
    // spi_device_transmit usa la ISR, también al lanzar el segundo bloque.
    for (unsigned n : {32768u, 8192u}) {
      spi_transaction_t t = {};
      t.length = n * 8;
      t.tx_buffer = datos;
      if (spi_device_transmit(dispositivo, &t) != ESP_OK) {
        ok = false;
        break;
      }
    }
    // RX real mediante GDMA, sin usar SPI_TRANS_USE_RXDATA (ruta de CPU).
    if (ok) {
      memset(datos, 0, 4);
      spi_transaction_t t = {};
      t.length = 32;
      t.rxlength = 32;
      t.rx_buffer = datos;
      ok = spi_device_transmit(dispositivo, &t) == ESP_OK;
      if (ok) {
        Serial.printf("S3-SPI-MISO=%02x%02x%02x%02x\n", datos[0], datos[1], datos[2], datos[3]);
        ok = datos[0] == 255 && datos[1] == 255 && datos[2] == 255 && datos[3] == 255;
      }
    }
    heap_caps_free(datos);
  }
  spi_bus_remove_device(dispositivo);
  spi_bus_free(SPI2_HOST);
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  SPI.begin(SCK_SPI, MISO_SPI, MOSI_SPI, -1);
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  uint8_t respuesta = SPI.transfer('a');
  uint8_t resto[] = {'b', 'c', 'd'};
  SPI.writeBytes(resto, sizeof(resto));
  SPI.endTransaction();
  SPI.end();
  if (respuesta != 255 || !dma_prueba()) {
    Serial.println("TALLER-FALLO s3_spi");
    return;
  }
  Serial.println("TALLER-OK s3_spi");
}

void loop() {
  delay(1000);
}
