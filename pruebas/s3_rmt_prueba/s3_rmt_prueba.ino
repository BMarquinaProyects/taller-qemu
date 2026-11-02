// ESP32-S3: RMT TX en GPIO10, 8 LED y después 30 LED con recarga ping-pong.
// FQBN: esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default
#include <Arduino.h>
#include "esp32-hal-rmt.h"

static rmt_data_t simbolos[30 * 24];

static bool enviar(unsigned leds, uint8_t rojo, uint8_t verde, uint8_t azul) {
  const uint8_t grb[] = {verde, rojo, azul};
  unsigned n = 0;
  for (unsigned led = 0; led < leds; led++) {
    for (uint8_t dato : grb) {
      for (int bit = 7; bit >= 0; bit--) {
        rmt_data_t &s = simbolos[n++];
        s.level0 = 1;
        s.duration0 = (dato & (1u << bit)) ? 8 : 4;
        s.level1 = 0;
        s.duration1 = (dato & (1u << bit)) ? 4 : 8;
      }
    }
  }
  // Un bloque S3 son 48 palabras: ambos trenes necesitan que la ISR recargue.
  // IDF añade el símbolo de fin; EOT bajo corresponde al reinicio WS2812.
  return rmtWrite(10, simbolos, n, 2000);
}

void setup() {
  Serial.begin(115200);  // UART0, con USB CDC desactivado.
  delay(1000);
  if (!rmtInit(10, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000) ||
      !rmtSetEOT(10, LOW) || !enviar(8, 17, 34, 51)) {
    Serial.println("TALLER-FALLO s3_rmt tren de 8 LED");
    return;
  }
  delay(1);  // Reposo bajo mayor que los 50 us de reinicio del WS2812.
  if (!enviar(30, 68, 85, 102)) {
    Serial.println("TALLER-FALLO s3_rmt tren de 30 LED");
    return;
  }
  Serial.println("TALLER-OK s3_rmt");
}

void loop() {
  delay(1000);
}
