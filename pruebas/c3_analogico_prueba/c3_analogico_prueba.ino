// ESP32-C3: ADC1 canal 0 (GPIO0) mediante el puente de Taller (trama ANALOGICO).
// FQBN: esp32:esp32:esp32c3:FlashMode=dio,FlashFreq=40,CDCOnBoot=default
// El verificador inyecta 2048 y luego 4095; el sketch confirma 0 → medio → alto.
#include <Arduino.h>

static int fase = 0;  // 0: espera 0, 1: espera medio, 2: espera alto, 3: hecho

void setup() {
  Serial.begin(115200);  // UART0; USB CDC debe estar desactivado.
  analogReadResolution(12);
}

void loop() {
  if (fase == 3) {
    delay(1000);
    return;
  }
  int v = analogRead(0);
  Serial.printf("C3-A=%d\n", v);
  if (fase == 0 && v == 0) fase = 1;
  else if (fase == 1 && v >= 1900 && v <= 2200) fase = 2;
  else if (fase == 2 && v >= 4000) {
    fase = 3;
    Serial.println("TALLER-OK c3_analogico");
  }
  delay(20);
}
