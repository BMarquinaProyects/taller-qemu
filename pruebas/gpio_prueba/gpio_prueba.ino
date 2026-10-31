// Prueba del GPIO emulado: lo que se escribe en un pin de salida debe poder leerse.
// Usa GPIO2 (registros OUT/ENABLE) y GPIO33 (registros OUT1/ENABLE1).

String fallos = "";

void comprobar(int pin, int nivel) {
  digitalWrite(pin, nivel);
  if (digitalRead(pin) != nivel) {
    fallos += " " + String(pin) + (nivel ? "H" : "L");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);
  pinMode(33, OUTPUT);
  comprobar(2, HIGH);
  comprobar(2, LOW);
  comprobar(33, HIGH);
  comprobar(33, LOW);
  if (fallos.length() == 0) {
    Serial.println("TALLER-OK gpio");
  } else {
    Serial.println("TALLER-FALLO gpio" + fallos);
  }
}

void loop() {
  delay(1000);
}
