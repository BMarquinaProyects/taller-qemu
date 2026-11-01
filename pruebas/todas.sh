#!/usr/bin/env bash
# Ejecuta las pruebas de humo con el binario indicado (por defecto, el de la compilación local en WSL).
set -o pipefail
Q="${1:-$HOME/qemu-trabajo/instalar-x86_64-linux-gnu/qemu/bin/qemu-system-xtensa}"
cd "$(dirname "$0")/.."
r=0
python3 pruebas/qemu_prueba.py "$Q" humo pruebas/taller_ok.flash.bin "TALLER-OK arduino" | head -3 || r=1
python3 pruebas/qemu_prueba.py "$Q" humo pruebas/gpio_prueba.flash.bin "TALLER-OK gpio" | head -3 || r=1
python3 pruebas/qemu_prueba.py "$Q" puente pruebas/puente_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" entrada pruebas/entrada_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" pwm pruebas/pwm_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" analogico pruebas/analogico_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" i2c pruebas/i2c_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" armado pruebas/armado_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" rmt pruebas/rmt_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" humo pruebas/rmt_prueba.flash.bin "TALLER-OK rmt" || r=1
MPY="${MICROPYTHON_FLASH:-pruebas/micropython.flash.bin}"
if [[ -f "$MPY" || -n "${MICROPYTHON_FLASH:-}" ]]; then
  python3 pruebas/qemu_prueba.py "$Q" spi_dma "$MPY" || r=1
else
  echo 'OMITIDA spi_dma: preparar pruebas/micropython.flash.bin (ver README)'
fi
exit $r
