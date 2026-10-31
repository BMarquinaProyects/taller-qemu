#!/usr/bin/env bash
# Ejecuta las tres pruebas de humo con el binario indicado (por defecto, el de la compilación local en WSL).
set -o pipefail
Q="${1:-$HOME/qemu-trabajo/instalar-x86_64-linux-gnu/qemu/bin/qemu-system-xtensa}"
cd "$(dirname "$0")/.."
r=0
python3 pruebas/qemu_prueba.py "$Q" humo pruebas/taller_ok.flash.bin "TALLER-OK arduino" | head -3 || r=1
python3 pruebas/qemu_prueba.py "$Q" humo pruebas/gpio_prueba.flash.bin "TALLER-OK gpio" | head -3 || r=1
python3 pruebas/qemu_prueba.py "$Q" puente pruebas/puente_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" entrada pruebas/entrada_prueba.flash.bin || r=1
exit $r
