#!/usr/bin/env bash
# Ejecuta las pruebas de humo con el binario indicado (por defecto, el de la compilación local en WSL).
set -o pipefail
Q="${1:-$HOME/qemu-trabajo/instalar-x86_64-linux-gnu/qemu/bin/qemu-system-xtensa}"
Q_C3="${2:-${QEMU_C3:-${Q/qemu-system-xtensa/qemu-system-riscv32}}}"
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
python3 pruebas/qemu_prueba.py "$Q" armado pruebas/armado_ocupado.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" rmt pruebas/rmt_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" fpu pruebas/fpu_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" humo pruebas/rmt_prueba.flash.bin "TALLER-OK rmt" || r=1
MPY="${MICROPYTHON_FLASH:-pruebas/micropython.flash.bin}"
if [[ -f "$MPY" || -n "${MICROPYTHON_FLASH:-}" ]]; then
  python3 pruebas/qemu_prueba.py "$Q" spi_dma "$MPY" || r=1
else
  echo 'OMITIDA spi_dma: preparar pruebas/micropython.flash.bin (ver README)'
fi
python3 pruebas/qemu_prueba.py "$Q_C3" c3_gpio pruebas/c3_gpio_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q_C3" c3_pwm pruebas/c3_pwm_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q_C3" c3_analogico pruebas/c3_analogico_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q_C3" c3_i2c pruebas/c3_i2c_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q_C3" c3_rmt pruebas/c3_rmt_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q_C3" c3_spi pruebas/c3_spi_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_gpio pruebas/s3_gpio_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_pwm pruebas/s3_pwm_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_analogico pruebas/s3_analogico_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_i2c pruebas/s3_i2c_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_rmt pruebas/s3_rmt_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_flash_qio pruebas/s3_flash_qio_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_spi pruebas/s3_spi_prueba.flash.bin || r=1
python3 pruebas/qemu_prueba.py "$Q" s3_fpu pruebas/s3_fpu_prueba.flash.bin || r=1
exit $r
