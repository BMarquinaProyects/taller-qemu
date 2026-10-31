void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
}

void loop() {
  Serial.print("A=");
  Serial.println(analogRead(34));
  delay(20);
}
