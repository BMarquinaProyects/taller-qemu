// Una salida de servo permite comprobar frecuencia y ciclo en el puente.
void setup() {
  Serial.begin(115200);
  ledcAttach(13, 50, 14);
  ledcWrite(13, 819);
  delay(100);
  Serial.println("TALLER-OK pwm");
}

void loop() {
  delay(1000);
}
