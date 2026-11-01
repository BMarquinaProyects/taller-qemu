#include <esp_cpu.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

static_assert(F_CPU == 240000000, "Esta prueba mide ciclos a 240 MHz");
static constexpr uint32_t CICLOS_US = 240;

// CCOUNT tiene resolución de un ciclo. Leer GPIO directamente acorta el bucle
// y evita la cuantización a microsegundos y el coste de micros(). Se fechan
// ambos flancos con el mismo bucle; el script exige un error máximo de ±3 us.
static bool esperarNivel4(bool alto, uint32_t &instante) {
  uint32_t inicio = esp_cpu_get_cycle_count();
  while (bool(REG_READ(GPIO_IN_REG) & (1u << 4)) != alto) {
    if (uint32_t(esp_cpu_get_cycle_count() - inicio) > 3000 * CICLOS_US) {
      return false;
    }
  }
  instante = esp_cpu_get_cycle_count();
  return true;
}

static bool medirDht(const char *etiqueta) {
  uint32_t flancos[5];
  bool ok = true;
  noInterrupts();
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  delayMicroseconds(1100);
  pinMode(4, INPUT_PULLUP);
  for (unsigned i = 0; i < 5; i++) {
    if (!esperarNivel4(i & 1, flancos[i])) {
      ok = false;
      break;
    }
  }
  // Esperar también la liberación final: el tren debe terminar antes de repetir.
  uint32_t fin;
  if (ok) {
    ok = esperarNivel4(true, fin);
  }
  interrupts();
  if (!ok) {
    Serial.printf("TALLER-FALLO armado %s timeout\n", etiqueta);
    return false;
  }
  uint32_t us[4];
  for (unsigned i = 0; i < 4; i++) {
    us[i] = (uint32_t(flancos[i + 1] - flancos[i]) + CICLOS_US / 2) / CICLOS_US;
  }
  Serial.printf("%s=%lu,%lu,%lu,%lu\n", etiqueta,
                (unsigned long)us[0], (unsigned long)us[1],
                (unsigned long)us[2], (unsigned long)us[3]);
  return true;
}

void setup() {
  Serial.begin(115200);
  // Habilitar una salida inicialmente baja también es un flanco de bajada.
  // El script espera esta marca antes de armar: no debe contar ese flanco.
  pinMode(5, OUTPUT);
  digitalWrite(5, LOW);
  pinMode(18, INPUT);
  Serial.println("ARMADO-LISTO");
  delay(1000);  // El script recibe HOLA e inyecta ambos armados antes de leer.

  if (!medirDht("DHT") || !medirDht("DHT2")) {
    return;
  }

  noInterrupts();
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  delayMicroseconds(300);
  pinMode(4, INPUT_PULLUP);
  bool sinRespuesta = true;
  uint32_t inicio = esp_cpu_get_cycle_count();
  while (uint32_t(esp_cpu_get_cycle_count() - inicio) < 500 * CICLOS_US) {
    if (!(REG_READ(GPIO_IN_REG) & (1u << 4))) {
      sinRespuesta = false;
    }
  }
  interrupts();
  Serial.println(sinRespuesta ? "CORTO=SIN_RESPUESTA" : "TALLER-FALLO armado CORTO");
  if (!sinRespuesta) {
    return;
  }

  noInterrupts();
  digitalWrite(5, HIGH);
  delayMicroseconds(10);
  digitalWrite(5, LOW);
  unsigned long eco = pulseIn(18, HIGH, 30000);
  interrupts();
  Serial.printf("ECO=%lu\n", eco);

  noInterrupts();
  digitalWrite(5, HIGH);
  delayMicroseconds(5);
  digitalWrite(5, LOW);
  unsigned long ecoCorto = pulseIn(18, HIGH, 30000);
  interrupts();
  Serial.printf("ECO_CORTO=%lu\n", ecoCorto);
  Serial.println("TALLER-OK armado");
}

void loop() {
  delay(1000);
}
