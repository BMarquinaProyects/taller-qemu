#include <Adafruit_NeoPixel.h>
#include "esp32-hal-rmt.h"

Adafruit_NeoPixel tira8(8, 13, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel tira30(30, 13, NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);
  delay(1000);
  tira8.begin();
  tira30.begin();
  // Adafruit usa exactamente esta configuración. Inicializar antes de medir
  // excluye el delay(1) de rmtInit del tiempo de la transmisión.
  if (!rmtInit(13, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)) {
    Serial.println("TALLER-FALLO rmtInit");
    return;
  }
  const uint8_t colores[8][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
    {0, 0, 0}, {1, 2, 3}, {128, 64, 32}, {255, 255, 0}
  };
  for (unsigned i = 0; i < 8; i++) {
    tira8.setPixelColor(i, colores[i][0], colores[i][1], colores[i][2]);
  }
  const uint32_t inicio = micros();
  tira8.show();
  const uint32_t duracion = micros() - inicio;
  Serial.printf("T_SHOW=%lu\n", (unsigned long)duracion);
  for (unsigned i = 0; i < 30; i++) {
    tira30.setPixelColor(i, i * 7, 255 - i * 5, i * 3);
  }
  tira30.show();
  Serial.println("TALLER-OK rmt");
}

void loop() {
  delay(1000);
}
