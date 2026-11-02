// S3 en QIO con la flash de 16 MiB (IS25LP128): borra un sector, escribe un patrón y lo lee.
// FQBN: esp32:esp32:esp32s3:FlashMode=qio,FlashSize=4M,PSRAM=disabled,CDCOnBoot=default
// Regresión del desfase de 2 bytes en las lecturas QIO (dummy de 0xEB en bytes, no en ciclos).
#include <Arduino.h>
#include "esp_flash.h"

static const uint32_t DIR = 0x300000;  // 3 MiB: fuera de la aplicación

void setup() {
  Serial.begin(115200);
  delay(500);
  uint8_t leido[16], patron[16];
  for (int i = 0; i < 16; i++) patron[i] = 0xA0 + i;
  esp_err_t e1 = esp_flash_erase_region(NULL, DIR, 4096);
  esp_flash_read(NULL, leido, DIR, 16);
  Serial.printf("BORRADO err=%d primeros=%02x %02x %02x %02x\n", e1, leido[0], leido[1], leido[2], leido[3]);
  esp_err_t e2 = esp_flash_write(NULL, patron, DIR, 16);
  memset(leido, 0, 16);
  esp_flash_read(NULL, leido, DIR, 16);
  Serial.printf("ESCRITO err=%d primeros=%02x %02x %02x %02x igual=%d\n", e2, leido[0], leido[1], leido[2], leido[3],
                memcmp(leido, patron, 16) == 0);
  uint32_t tam = 0;
  esp_flash_get_size(NULL, &tam);
  uint32_t fisico = 0, id = 0;
  esp_flash_get_physical_size(NULL, &fisico);
  esp_flash_read_id(NULL, &id);
  Serial.printf("TAMANO=%u FISICO=%u ID=%06x\n", (unsigned)tam, (unsigned)fisico, (unsigned)id);
  Serial.println("TALLER-OK flash");
}

void loop() { delay(1000); }
