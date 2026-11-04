// ENTRADA_TREN (puente v8): rebote simulado en GPIO16 con INPUT_PULLUP. UART0 y flash DIO.
// El GPIO del QEMU no genera IRQ, así que se cuentan los flancos leyendo en un bucle cerrado.
#include <Arduino.h>

static const unsigned PIN = 16;
static bool bien = true;

// Espera un nivel hasta `plazo_ms`; devuelve si llegó.
static bool esperar_nivel(int nivel, uint32_t plazo_ms) {
  uint32_t inicio = millis();
  while (digitalRead(PIN) != nivel && millis() - inicio < plazo_ms) {
  }
  return digitalRead(PIN) == nivel;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN, INPUT_PULLUP);
  delay(5);
  int inicial = digitalRead(PIN);
  Serial.printf("TREN-LISTO\n");
  Serial.printf("TREN-INICIAL=%d\n", inicial);
  bien &= inicial == 1;

  // Tren: 0 (0 µs), suelto (300 µs), 0 (300 µs), suelto (300 µs), 0 (300 µs).
  // El primer 0 marca el inicio; después vienen 4 flancos y acaba en 0.
  bien &= esperar_nivel(LOW, 10000);
  int anterior = LOW, flancos = 0;
  uint32_t inicio = micros();
  while (micros() - inicio < 50000) {
    int ahora = digitalRead(PIN);
    if (ahora != anterior) {
      flancos++;
      anterior = ahora;
    }
  }
  int final = digitalRead(PIN);
  Serial.printf("FLANCOS=%d\n", flancos);
  Serial.printf("NIVEL=%d\n", final);
  bien &= flancos == 4 && final == 0;

  // Cancelación: el verificador manda un tren «suelto (0 µs), 0 (20 ms)» y
  // enseguida ENTRADA=1. El paso de 20 ms no debe llegar nunca.
  Serial.printf("TREN-CANCELA-LISTO\n");
  bien &= esperar_nivel(HIGH, 10000);
  delay(40);
  int cancelado = digitalRead(PIN);
  Serial.printf("CANCELADO=%d\n", cancelado);
  bien &= cancelado == 1;
  Serial.printf(bien ? "TALLER-OK tren\n" : "TALLER-FALLO tren\n");
}

void loop() {
  delay(1000);
}
