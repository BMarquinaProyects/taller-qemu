// ESP32-C3: Wire (I2C0 hardware) con el espejo del puente en 0x3C.
// FQBN: esp32:esp32:esp32c3:FlashMode=dio,FlashFreq=40,CDCOnBoot=default
#include <Arduino.h>
#include <Wire.h>

void setup() {
  Serial.begin(115200);  // UART0, con USB CDC desactivado.
  if (!Wire.begin(4, 5, 100000)) {  // SDA4/SCL5, seleccionados por la matriz.
    Serial.println("TALLER-FALLO c3_i2c inicio");
    return;
  }
  delay(1000);  // El receptor externo da de alta 0x3C después de HOLA.

  Wire.beginTransmission(0x3C);
  const uint8_t sondeo = Wire.endTransmission();
  Wire.beginTransmission(0x3C);
  Wire.write(0xD0);
  if (sondeo != 0 || Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(uint8_t(0x3C), size_t(1)) != 1) {
    Serial.println("TALLER-FALLO c3_i2c lectura con RSTART");
    return;
  }
  const int id = Wire.read();
  Serial.printf("C3-I2C-ID=%02X\n", id);

  Wire.beginTransmission(0x3C);
  Wire.write(0x00);
  Wire.write(0xAF);
  const uint8_t ack = Wire.endTransmission();
  Serial.printf("C3-I2C-ACK=%u\n", ack);
  Wire.beginTransmission(0x3D);  // Sin alta: debe responder NACK de dirección.
  const uint8_t nack = Wire.endTransmission();
  Serial.printf("C3-I2C-NACK=%u\n", nack);
  Serial.println(id == 0xAB && ack == 0 && nack == 2 ?
                 "TALLER-OK c3_i2c" : "TALLER-FALLO c3_i2c");
}

void loop() {
  delay(1000);
}
