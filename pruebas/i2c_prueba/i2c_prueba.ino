#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();  // ESP32 clásico: SDA 21, SCL 22, bus 0.
  delay(1000);   // El programa externo registra las direcciones tras HOLA.

  for (uint8_t direccion = 0x08; direccion <= 0x77; direccion++) {
    Wire.beginTransmission(direccion);
    if (Wire.endTransmission() == 0) {
      Serial.printf("ENCONTRADO 0x%02X\n", direccion);
    }
  }

  Wire.beginTransmission(0x3C);
  Wire.write(0x00);
  Wire.write(0xAF);
  if (Wire.endTransmission() != 0) {
    Serial.println("TALLER-FALLO i2c escritura");
    return;
  }

  Wire.beginTransmission(0x3C);
  Wire.write(0xD0);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(uint8_t(0x3C), size_t(2)) != 2) {
    Serial.println("TALLER-FALLO i2c lectura");
    return;
  }
  int primero = Wire.read();
  int segundo = Wire.read();
  Serial.printf("LEIDO=%02X,%02X\n", primero, segundo);
  Serial.println("TALLER-OK i2c");
}

void loop() {
  delay(1000);
}
