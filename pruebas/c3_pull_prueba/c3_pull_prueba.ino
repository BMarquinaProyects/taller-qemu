// M-239: contactos pasivos; UART0 y flash DIO.
#include <Arduino.h>

static bool bien = true;

static void esperar(char comando) {
  uint32_t inicio = millis();
  while (millis() - inicio < 10000) {
    if (Serial.available() && Serial.read() == comando) {
      return;
    }
    delay(1);
  }
  bien = false;
}

// Esperar un cambio real evita depender del orden de atención de dos sockets.
static bool esperar_nivel(unsigned pin, int nivel) {
  uint32_t inicio = millis();
  while (digitalRead(pin) != nivel && millis() - inicio < 10000) {
    delay(1);
  }
  bool correcto = digitalRead(pin) == nivel;
  bien &= correcto;
  return correcto;
}

static void probar(unsigned pin, const char *prefijo) {
  pinMode(pin, INPUT_PULLUP);
  esperar_nivel(pin, HIGH);
  int arriba = digitalRead(pin);
  Serial.printf("%sPULL-ARRIBA=%d\n", prefijo, arriba);
  pinMode(pin, INPUT_PULLDOWN);
  delay(1);
  int abajo = digitalRead(pin);
  Serial.printf("%sPULL-ABAJO=%d\n", prefijo, abajo);
  pinMode(pin, INPUT);
  delay(1);
  int sin = digitalRead(pin);
  Serial.printf("%sSIN-PULL=%d\n", prefijo, sin);
  bien &= arriba == 1 && abajo == 0 && sin == 0;
}

void setup() {
  Serial.begin(115200); // USB CDC desactivado en C3/S3.
  pinMode(9, INPUT);
  Serial.printf("PULL-LISTO\n");
  esperar('C'); // El verificador está imponiendo cero.
  bool semilla = true;
  semilla &= esperar_nivel(9, LOW);
  Serial.printf("PULL-SEMILLA=%d\n", semilla ? 0 : 1);
  esperar('S'); // ENTRADA 2 se envía en orden inverso; el primer pad es el último.
  probar(9, "");
  pinMode(9, INPUT_PULLUP);
  Serial.printf("PULL-IMPUESTO-LISTO\n");
  esperar('I'); // El verificador está imponiendo cero, con el pull-up activo.
  esperar_nivel(9, LOW);
  int impuesto = digitalRead(9);
  Serial.printf("IMPUESTO=%d\n", impuesto);
  bien &= impuesto == 0;
  Serial.printf(bien ? "TALLER-OK c3_pull\n" : "TALLER-FALLO c3_pull\n");
}

void loop() {
  delay(1000);
}
