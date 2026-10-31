// Prueba de entradas del puente de Taller: el programa externo cambia el nivel de GPIO15
// y el sketch lo lee con digitalRead (con pull-up: suelto = 1).
void setup() {
  Serial.begin(115200);
  pinMode(15, INPUT_PULLUP);
}

void loop() {
  static unsigned long inicio = millis();
  Serial.print("NIVEL=");
  Serial.println(digitalRead(15));
  if (millis() - inicio > 3000) {
    Serial.println("TALLER-OK entrada");
    while (true) delay(1000);
  }
  delay(20);
}
