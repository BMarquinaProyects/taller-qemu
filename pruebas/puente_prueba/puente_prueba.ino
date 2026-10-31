// Prueba del puente de Taller: cambios de GPIO y bytes de SPI deben llegar al programa externo.
#include <SPI.h>

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);
  pinMode(33, OUTPUT);
  digitalWrite(2, HIGH);
  digitalWrite(33, HIGH);

  SPI.begin();  // VSPI: SCK 18, MISO 19, MOSI 23
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  uint8_t cabecera[] = {0xA5, 0x5A, 0x00, 0xFF};
  SPI.writeBytes(cabecera, sizeof(cabecera));
  for (int i = 0; i < 1000; i++) {
    SPI.transfer(i & 0xFF);
  }
  SPI.endTransaction();

  digitalWrite(2, LOW);
  Serial.println("TALLER-OK puente");
}

void loop() {
  delay(1000);
}
