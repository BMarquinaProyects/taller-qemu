// ESP32-C3: LED GPIO8 y pulsador GPIO9 mediante el puente de Taller.
// FQBN: esp32:esp32:esp32c3:FlashMode=dio,FlashFreq=40,CDCOnBoot=default
#include <Arduino.h>

void setup() {
  Serial.begin(115200);  // UART0; USB CDC debe estar desactivado.
  pinMode(8, OUTPUT);
  digitalWrite(8, LOW);
  pinMode(9, INPUT_PULLUP);
  delay(200);
  for (unsigned i = 0; i < 3; i++) {
    digitalWrite(8, HIGH);
    delay(100);
    digitalWrite(8, LOW);
    delay(100);
  }

  int antes = digitalRead(9);
  Serial.printf("C3-ANTES=%d\n", antes);
  // El verificador envía ENTRADA(9, 0) después de leer C3-ANTES=1.
  uint32_t inicio = millis();
  while (digitalRead(9) != LOW && millis() - inicio < 10000) {
    delay(10);
  }
  int despues = digitalRead(9);
  Serial.printf("C3-DESPUES=%d\n", despues);
  Serial.println(antes == HIGH && despues == LOW ? "TALLER-OK c3" : "TALLER-FALLO c3");
}

void loop() {
  delay(1000);
}
