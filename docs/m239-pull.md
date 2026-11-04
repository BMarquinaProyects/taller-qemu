# M-239: contactos pasivos y puente v8

> Nota: en la bitácora de Taller esta mejora es **M-245** (M-239 acabó siendo el
> modelo del BME280 de 6 pines). El nombre de la rama y de este archivo se conserva.

Plan de implementación, según la especificación de M-239:

- [x] Modelar IO_MUX en los tres GPIO y RTC_IO en el clásico. Conservar registros
  completos, reset a cero y tabla de pads de ESP-IDF 5.5.6. Añadir máscaras
  `pull_arriba`, `pull_abajo`, `sueltos`; GPIO34–39 sin resistencias.
- [x] Aceptar ENTRADA 2 y resolver entradas sueltas al recibirla o cambiar pulls.
  Arriba gana si están ambos; sin pull se conserva la carga como simplificación
  determinista. Entradas impuestas y pasos armados retiran `sueltos`.
- [x] Conectar MMIO y alias de las máquinas; puente v8 con PULL LE de 16 bytes,
  solo ante cambios y una vez inmediatamente después de HOLA.
- [x] Probar modelos y emisor reales aislados, receptores y verificadores Python.
  Añadir sketches DIO y modos `pull`, `c3_pull`, `s3_pull` a `todas.sh`.
- [x] Actualizar README y ejecutar unitarias, py_compile, revisión del diff y LF.

Arquitectura: los GPIO poseen sus regiones MMIO adicionales, registros y reset.
Una cabecera pura comparte tablas y cálculo eléctrico; evita nuevos dispositivos
y cambios de Meson. El puente conserva las últimas máscaras incluso antes de HOLA.
Se mantiene el RTC_IO propio del S3 y la lógica de `taller_armado.h`.

Límites: solo este worktree, sin commits, VERSION ni workflows. Sin ejecutar ni
compilar QEMU. El revisor verifica enlace, arranque, Linux y binario Windows.

Revisión: alias APB del clásico, RTC_IO compartido de GPIO32/33, máscaras altas
en Windows (E-045), huecos S3, reset de entradas sueltas, orden HOLA/PULL y
compatibilidad de verificadores existentes ante el tipo nuevo.

Verificación: 160 unitarias ejecutadas con `PYTHONIOENCODING=utf-8`: 151 pasan
y 9 se omiten por requerir GCC/Clang. Los ocho casos nuevos pasan, incluidos
los tres modelos y el emisor reales compilados con MSVC C11, además de los
receptores y armados anteriores. `py_compile` y `git diff --check` sin errores.
Tres sketches compilados con CLI 1.5.1 / Arduino ESP32 3.3.12; imágenes DIO
fusionadas de 4 MiB, arranque en 0x1000 (clásico) o 0 (C3/S3).

Revisión independiente sin defectos críticos/importantes en los modelos. Se
corrigió un riesgo de orden entre sockets en los modos de prueba: TIEMPO no
sirve como acuse de ENTRADA porque puede haber latidos pendientes. El firmware
confirma la semilla cero, espera la subida al soltar con PU y espera la bajada
al imponer cero. Regresión de sockets con latidos pendientes: falla antes del
cambio y pasa después. No cambia el protocolo ni el comportamiento de GPIO.

Decisiones de alcance: selección RTC/digital y descarga física quedan en las
simplificaciones pedidas; IRQ GPIO, otros niveles inválidos y reconexión del
puente mantienen el comportamiento previo. No se modifican subsistemas ajenos.
La compilación y ejecución de QEMU siguen pendientes del revisor.

## Revisión (2026-11-04)

- Compilado en WSL (Ubuntu 24.04) con las capas: `todas.sh` pasa entero, incluidos
  `pull`, `c3_pull` y `s3_pull`.
- Las 9 unitarias que se omitían sin GCC (modelos reales de los tres GPIO y los
  receptores) pasan con GCC en Linux.
- Añadido **ENTRADA_TREN** (tipo 21, también v8), ver README: el núcleo ya la
  enviaba al ver HOLA v8; sin ella, el estado final de un contacto no llegaba.
