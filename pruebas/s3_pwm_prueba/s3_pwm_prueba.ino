// ESP32-S3: LEDC LS en GPIO4, 50 Hz al 5 %, al 25 %, 100 Hz y apagado.
// FQBN: esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default
#include <Arduino.h>

void setup() {
  Serial.begin(115200);  // UART0, USB CDC desactivado.
  bool ok = ledcAttach(4, 50, 14);
  ledcWrite(4, 819);
  delay(100);
  ledcWrite(4, 4096);
  delay(100);
  ok = ok && ledcChangeFrequency(4, 100, 14);
  delay(100);
  ledcWrite(4, 0);
  delay(100);
  Serial.println(ok ? "TALLER-OK s3_pwm" : "TALLER-FALLO s3_pwm");
}

void loop() {
  delay(1000);
}
