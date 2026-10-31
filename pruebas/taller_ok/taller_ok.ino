// Fase 0: prueba mínima de arranque en QEMU (ESP32 clásico).
// Imprime la marca de éxito y hace parpadear GPIO2 cada 500 ms.

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);
}

void loop() {
  Serial.println("TALLER-OK arduino");
  digitalWrite(2, !digitalRead(2));
  delay(500);
}
