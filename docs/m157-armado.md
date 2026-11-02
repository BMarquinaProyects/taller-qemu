# M-157: plazos del tren y tiempos observados

Estado: plazos comprobados con lógica y GPIO reales, sin ejecutar QEMU.
No hay corrección funcional ni causa de CI confirmada. Se conservan sketch,
imagen flash y tolerancia de ±3 µs. Referencias de partida: E-054, M-157 y E-041
de la bitácora de Taller, leídas antes de investigar.

## Qué demuestra el código

- `capas/hw/gpio/esp32_gpio.c:201`: la escritura MMIO cambia OUT/ENABLE,
  envía GPIO al puente y observa el disparo en esa misma llamada.
  `esp32_gpio_observar_armados` toma `QEMU_CLOCK_VIRTUAL` (línea 81).
  No se recibe el disparo por una respuesta del anfitrión.
- `capas/include/hw/misc/taller_armado.h:171`: primer plazo = instante virtual
  del disparo + primera espera. Línea 195: cada plazo siguiente = plazo
  **previsto anterior** + espera. Ya está implementada la ausencia de deriva.
- `esp32_gpio.c:252`: temporizadores en `QEMU_CLOCK_VIRTUAL`, no REALTIME/HOST.
  Líneas 66–75: callback toma el ahora virtual, aplica todos los pasos vencidos
  y rearma al próximo plazo previsto. No puede aplicar un flanco retroactivamente:
  `in_externo` cambia cuando se ejecuta `esp32_gpio_fijar_entrada`.
  Si vencieron varios pasos, quedan aplicados sin ejecutar firmware entre ellos.
- `esp32_gpio.c:143`: leer IN consulta el estado actual; no reconstruye el
  nivel que habría correspondido a un plazo anterior.
- `pruebas/armado_prueba/armado_prueba.ino:11`: sondeo directo de GPIO;
  línea 18: CCOUNT se lee **después** de detectar el nivel. Línea 48: resta
  entre esas muestras y redondeo a µs, con 240 ciclos/µs. No usa `micros()`.
  Un ciclo son unos 4,17 ns; redondear aporta hasta medio µs, no explica por sí
  solo un error de 4 µs. ECHO usa `pulseIn` (línea 92).

En la base exacta `esp-develop-9.2.2-20260417`, `HELPER(update_ccount)` convierte
el mismo reloj virtual a ticks de CPU; no utiliza directamente reloj de pared.
Véase [op_helper.c, líneas 36–44](https://github.com/espressif/qemu/blob/esp-develop-9.2.2-20260417/target/xtensa/op_helper.c#L36-L44).

Con `shift=2`, icount asigna 4 ns por instrucción, no 240 MHz de instrucciones.
`icount_start_warp_timer` exige todas las CPU ociosas; con `sleep=on`,
`icount_warp_rt` incorpora tiempo de anfitrión al sesgo virtual durante ese reposo.
Capturar DHT es un bucle activo, por lo que una pausa del proceso anfitrión no
demuestra por sí sola un salto virtual dentro de esa captura. El arranque y
`delay(1000)` sí atraviesan reposos y pueden cambiar la fase de ejecución.
Véase [icount-common.c, líneas 145–147 y 215–360](https://github.com/espressif/qemu/blob/esp-develop-9.2.2-20260417/accel/tcg/icount-common.c#L215-L360).

Hay dos CPU por defecto (`capas/hw/xtensa/esp32.c:1001`). Desactivar
interrupciones locales no detiene la otra CPU. QEMU reparte el presupuesto
icount entre CPU; sigue pendiente comprobar si eso separa la lectura GPIO y
la lectura CCOUNT, o retrasa el siguiente sondeo en el caso fallido.
Es una hipótesis de medida, no una causa probada.
Véase [tcg-accel-ops-icount.c, líneas 85–95](https://github.com/espressif/qemu/blob/esp-develop-9.2.2-20260417/accel/tcg/tcg-accel-ops-icount.c#L85-L95).

## De dónde pueden salir ambos signos

Sean `P[i]` el plazo previsto, `L[i]` el retraso hasta aplicar el flanco y
`S[i]` el retraso adicional hasta fecharlo en el firmware. Antes del redondeo:

```text
medida[i] = P[i+1] - P[i] + L[i+1] - L[i] + S[i+1] - S[i]
```

Los plazos DHT desde el disparo son 30, 110, 190, 240, 310 y 360 µs.
Un retraso aislado puede alargar un ancho y acortar el siguiente, sin deriva:

| Atención impuesta en la unitaria | Primer ancho | Cuarto ancho | Cola final |
|---|---:|---:|---:|
| Primer flanco en 34 µs; resto puntual | 76 µs | 70 µs | 50 µs |
| Flanco de 310 µs en 314 µs; resto puntual | 80 µs | 74 µs | 46 µs |

`pruebas/armado_gpio_modelo.c` compila las rutas MMIO, callback y lectura IN
**reales** del clásico con dobles de reloj/temporizador/puente. Comprueba esos
dos casos y que todos los rearme conservan los plazos originales. Explica los
valores comunicados; el retraso de 4 µs es una entrada impuesta a la prueba,
no una reproducción ni medición de lo ocurrido en CI.

`pruebas/armado_logica.c` cubre seis atenciones con retrasos distintos y una
atención que encuentra cuatro pasos vencidos. El quinto mantiene su plazo.
Se verificó que ambas pruebas detectan una mutación que suma la espera a
`ahora_ns` en lugar de `r->proximo_ns`, usando copias temporales del código.
El verificador sigue rechazando expresamente 76 y DHT2=74.

## Propuesta para cerrar el diagnóstico

Antes de modificar el modelo o usar medianas, capturar en una ejecución fallida:

1. Instante del disparo en `esp32_gpio_observar_armados`.
2. En cada paso: índice, `r->proximo_ns` **antes** de avanzar, ahora virtual
   del callback y nivel aplicado. Con varios vencidos, registrar cada paso.
3. En IN, solo cuando cambia el nivel leído: tiempo virtual, nivel y CPU.
   Guardar también los CCOUNT crudos del sketch y escribirlos después de
   `interrupts()`, sin Serial dentro de la captura.

Si falla 2, investigar entrega de temporizadores QEMU. Si 2 es puntual y
falla 3 o CCOUNT, investigar reparto entre CPU y separación GPIO/CCOUNT.
Una comparación controlada con `sleep=off` puede separar la fase debida al
reposo; no debe sustituir sin más la configuración de CI. Un registro en
memoria evita bloquear el puente durante los pulsos; volcarlo al terminar.

E-041 prueba entrega irregular de latidos fechados al enviarse; no demuestra
que DHT tenga la misma causa. A diferencia de un latido, perder un pulso afecta
al firmware. Tampoco bastaría poner fechas previstas en una traza o descartar
muestras fallidas para corregir el nivel visto por la CPU.

## Comprobación

```sh
python3 -m unittest discover -s pruebas -p '*_unitarias.py'
python3 pruebas/qemu_prueba.py /ruta/qemu-system-xtensa armado pruebas/armado_prueba.flash.bin
```

En Windows, `armado_unitarias.py` también admite `cl` desde consola de
desarrollo de Visual Studio. Las pruebas nuevas ejecutan C real con MSVC.
Resultado local: 141 pruebas, 132 ejecutadas correctamente y 9 omitidas por
arneses anteriores que buscan GCC/Clang. `PYTHONUTF8=1` evita errores de consola
CP1252 en las pruebas de sockets C3/S3; no se cambió código para esos errores.

Para medir frecuencia, repetir el segundo comando 100 veces por plataforma,
guardar cada salida y contar códigos distintos de cero; repetir con carga de
anfitrión comparable. Esta entrega no cambia el comportamiento de QEMU ni del
firmware: no se afirma una mejora de frecuencia. Quedan pendientes ejecución
real, trazas y eventual corrección. No se recompiló la flash porque no cambió
el sketch; no se tocó `VERSION`, workflows ni se hicieron commits.
