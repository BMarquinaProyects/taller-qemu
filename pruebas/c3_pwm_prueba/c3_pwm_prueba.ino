// ESP32-C3: LEDC en GPIO8 mediante el puente de Taller (trama PWM).
// FQBN: esp32:esp32:esp32c3:FlashMode=dio,FlashFreq=40,CDCOnBoot=default
// Secuencia: 50 Hz al 5 % (servo), 50 Hz al 25 %, 100 Hz al 25 % y apagado.
#include <Arduino.h>

void setup() {
  Serial.begin(115200);  // UART0; USB CDC debe estar desactivado.
  bool ok = ledcAttach(8, 50, 14);
  ledcWrite(8, 819);   // 819 / 16384 = 5 %
  delay(100);
  ledcWrite(8, 4096);  // 25 %
  delay(100);
  ok = ok && ledcChangeFrequency(8, 100, 14);
  delay(100);
  ledcWrite(8, 0);
  delay(100);
  Serial.println(ok ? "TALLER-OK c3_pwm" : "TALLER-FALLO c3_pwm");
}

void loop() {
  delay(1000);
}
