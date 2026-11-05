// Prueba de UNHILO (v9): dos DS18B20 en GPIO4 con las librerías OneWire y DallasTemperature.
// El script da de alta los dos sensores tras «UNHILO-LISTO» y comprueba lo que se imprime.
// CPU1 está siempre ocupada: sin el cuanto corto durante el 1-Wire (M-157), su turno alargaría
// las ranuras de CPU0 y algún bit escrito se leería mal.
#include <OneWire.h>
#include <DallasTemperature.h>

OneWire bus(4);
DallasTemperature sensores(&bus);

void imprimir(const char *etiqueta) {
  sensores.requestTemperatures();
  for (int i = 0; i < sensores.getDeviceCount(); i++) {
    DeviceAddress a;
    if (!sensores.getAddress(a, i)) continue;
    Serial.printf("%s ROM=", etiqueta);
    for (int k = 0; k < 8; k++) Serial.printf("%02X", a[k]);
    Serial.printf(" T=%.4f\n", sensores.getTempC(a));
  }
}

static void ocupar(void *) {
  volatile uint32_t x = 0;
  for (;;) x++;
}

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(ocupar, "ocupar", 2048, nullptr, 1, nullptr, 1);
  pinMode(4, INPUT_PULLUP);
  delay(200);
  Serial.println("UNHILO-LISTO");
  delay(1500);
  sensores.begin();
  Serial.printf("N=%d\n", sensores.getDeviceCount());
  Serial.printf("PARASITO=%d\n", sensores.isParasitePowerMode());
  imprimir("T12");
  sensores.setResolution(9);
  imprimir("T9");
  Serial.println("TALLER-OK unhilo");
}

void loop() {
  delay(1000);
}
