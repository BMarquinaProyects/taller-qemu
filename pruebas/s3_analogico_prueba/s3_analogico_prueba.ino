// ESP32-S3: ADC1 canal 0, GPIO1. ANALOGICO inyecta valores crudos de 12 bits.
// FQBN: esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default
#include <Arduino.h>

static int fase = 0;

void setup() {
  Serial.begin(115200);  // UART0, USB CDC desactivado.
  analogReadResolution(12);
}

void loop() {
  if (fase == 3) {
    delay(1000);
    return;
  }
  int v = analogRead(1);
  Serial.printf("S3-A=%d\n", v);
  if (fase == 0 && v == 0) fase = 1;
  else if (fase == 1 && v >= 1900 && v <= 2200) fase = 2;
  else if (fase == 2 && v >= 4000 && v <= 4095) {
    fase = 3;
    Serial.println("TALLER-OK s3_analogico");
  }
  delay(20);
}
