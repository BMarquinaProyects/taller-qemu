// ESP32-S3: tres pulsos GPIO2 y entrada GPIO4 mediante el puente de Taller.
// FQBN: esp32:esp32:esp32s3:FlashMode=dio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default
#include <Arduino.h>

void setup() {
  Serial.begin(115200);  // UART0, USB CDC desactivado.
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
  pinMode(4, INPUT_PULLUP);
  delay(200);
  for (unsigned i = 0; i < 3; i++) {
    digitalWrite(2, HIGH);
    delay(100);
    digitalWrite(2, LOW);
    delay(100);
  }
  int antes = digitalRead(4);
  Serial.printf("S3-ANTES=%d\n", antes);
  // El verificador envía ENTRADA(4, 0) después de observar S3-ANTES=1.
  uint32_t inicio = millis();
  while (digitalRead(4) != LOW && millis() - inicio < 10000) {
    delay(10);
  }
  int despues = digitalRead(4);
  Serial.printf("S3-DESPUES=%d\n", despues);
  Serial.println(antes == HIGH && despues == LOW ? "TALLER-OK s3_gpio" : "TALLER-FALLO s3_gpio");
}

void loop() {
  delay(1000);
}
