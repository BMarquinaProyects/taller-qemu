# M-157: plazos del tren y tiempos observados

Estado actualizado: diagnóstico confirmado por el revisor en QEMU real.
Se añade un cuanto virtual durante la preparación acotada, los trenes del
clásico y del S3 y una cola de 20 µs después del último paso; se conservan el
sketch original, las imágenes flash y la tolerancia de ±3 µs. La sección
«Conclusión» recoge el diagnóstico, el arreglo y su verificación pendiente en
QEMU. El análisis inicial se conserva debajo; sus referencias de línea
corresponden al código anterior al cuanto. Referencias de partida: E-054,
M-157 y E-041 de la bitácora de Taller, leídas antes de investigar.

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
icount entre CPU. La hipótesis inicial era que ese reparto separase la lectura
GPIO de CCOUNT o retrasase el siguiente sondeo. El revisor ha confirmado
el retraso del sondeo mientras ejecuta la CPU0; se detalla en «Conclusión».
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

## Propuesta inicial para cerrar el diagnóstico

Se conserva el plan de investigación previo; el diagnóstico ya está cerrado
con la reproducción y el experimento descritos en «Conclusión». Se proponía,
antes de modificar el modelo o usar medianas, capturar en una ejecución fallida:

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
Resultado local de la investigación anterior: 141 pruebas, 132 ejecutadas correctamente y 9 omitidas por
arneses anteriores que buscan GCC/Clang. `PYTHONUTF8=1` evita errores de consola
CP1252 en las pruebas de sockets C3/S3; no se cambió código para esos errores.

Para medir frecuencia, repetir el segundo comando 100 veces por plataforma,
guardar cada salida y contar códigos distintos de cero; repetir con carga de
anfitrión comparable. La entrega inicial no cambió el comportamiento de QEMU
ni del firmware y no afirmó una mejora de frecuencia: quedaron pendientes
la ejecución real y el diagnóstico, ahora aportados por el revisor. No se
recompiló la flash porque no cambió el sketch; no se tocó `VERSION`, workflows
ni se hicieron commits.

## Conclusión

El revisor confirmó en QEMU real que E-054 procede del reparto de las dos CPU
bajo `-icount`, no de una deriva en los plazos del tren. En
`accel/tcg/tcg-accel-ops-rr.c`, ambas CPU ejecutan por turnos en un único hilo.
En cada vuelta, `icount_handle_deadline()` aplica los temporizadores vencidos
y cada CPU recibe `icount_percpu_budget(n) = limite / n` instrucciones; el
límite depende del tiempo restante hasta el próximo plazo de
`QEMU_CLOCK_VIRTUAL`. Las instrucciones de cualquiera de las CPU avanzan ese
reloj y, con él, el CCOUNT observado por el firmware.

Después de aplicar un flanco, CPU0 ejecuta primero. Arduino sondea desde CPU1,
donde corren `setup()` y `loop()`, y no ve el nivel hasta terminar el turno de
CPU0. Si CPU0 está en `waiti`, no añade retraso; si atiende el tick de FreeRTOS,
cientos de instrucciones a 4 ns por instrucción separan el plazo del sondeo
unos µs. Las interrupciones suspendidas en CPU1 no detienen ese trabajo en CPU0.

La reproducción determinista es `pruebas/armado_ocupado/armado_ocupado.ino`:
el sketch original con una tarea que ocupa CPU0 sin ceder. El revisor obtuvo
siempre `DHT=80,65,60,60` (esperado `80,80,50,70`) y `ECO=1083` (esperado
`1163..1169`). Un parche temporal que limitaba cada turno a 250 instrucciones,
1 µs con `shift=2`, hizo pasar tanto `armado` como `armado_ocupado`. Ese
experimento confirma la causa; el parche global de `accel/tcg` no se incorpora,
porque afectaría a todas las máquinas y limitaría los turnos aun en reposo.

El arreglo se limita al GPIO del clásico y del S3. Cada dispositivo crea un
temporizador adicional en `QEMU_CLOCK_VIRTUAL`, con periodo
`TALLER_ARMADO_CUANTO_NS = 1000`. Tras observar una escritura de GPIO, se programa
a ahora +1 µs si no está ya pendiente y la condición pura
`taller_armado_cuanto_activo()` indica actividad: preparación acotada, algún
tren sonando **o** `ahora_ns < fin_cola_ns`. El callback usa esa misma condición
para rearmarse a ahora +1 µs.
Así, el siguiente plazo está como máximo a 1 µs y cada turno de las dos CPU
queda acotado a 0,5 µs durante esas tres fases. Los pasos conservan su
temporizador y sus plazos encadenados desde el plazo previsto anterior;
el cuanto no aplica niveles ni modifica esos plazos.

La primera revisión de este cuanto pasó `todas.sh` y 20/20 ejecuciones de
`armado_prueba.flash.bin` en QEMU real. Con CPU0 ocupada corrigió DHT, pero ECO
falló siempre: 1408, 1409, 1432 y 1361 µs. El último paso baja ECHO a +1616 µs
y cierra la medida de `pulseIn` en CPU1. Al desaparecer el último tren, el
cuanto dejaba de rearmarse en esa misma llamada a `icount_handle_deadline()`;
el presupuesto volvía a depender del siguiente plazo lejano y CPU0 ejecutaba
unos 240 µs antes de que CPU1 viera la bajada. En DHT el último flanco solo se
espera y no cierra un ancho medido; por eso allí no aparecía este fallo.

Ahora cada último paso aplicado renueva `fin_cola_ns` a ahora virtual más
`TALLER_ARMADO_CUANTO_COLA_NS = 20000` (20 µs), con suma saturada. Se usa el
instante de aplicación, incluso si el callback llega tarde, para dar tiempo
a observar el último flanco. Si coinciden paso y tick, el tick anterior al
paso aún ve un tren y deja pendiente ahora +1 µs; si se ejecuta después, ve
la cola. Si el último paso cae entre ticks, el siguiente ya está pendiente.
Por tanto, al calcular el siguiente presupuesto queda un plazo a ≤1 µs.
Un tren de espera cero también deja cola desde su disparo.

Con esa cola, el revisor obtuvo `todas.sh` completa, 152 unitarias correctas y
20/20 ejecuciones de `armado_prueba`. `armado_ocupado` pasó 19/20: una ejecución
emitió dos DISPARO de GPIO5 y falló `ECO_CORTO=0`. El pulso que debía rechazarse
era `HIGH; delayMicroseconds(5); LOW`, con mínimo de 8 µs. Antes de ese disparo
no sonaba ningún tren: CPU0 podía consumir un turno largo entre el final de
la espera y la escritura LOW de CPU1. El ancho virtual superaba entonces 8 µs.
Es el mismo reparto por turnos, ahora durante la medida previa al disparo.

La preparación cubre esa fase: una ranura armada y sin tren cuenta si
`!!(s->conducido & bit) != p->subida` y la edad desde `s->cambio_ns[pin]` es
menor que `minimo_ns + TALLER_ARMADO_CUANTO_COLA_NS`. La comparación usa una
edad no negativa y un margen de 64 bits, sin sumar el margen al reloj absoluto.
Con mínimo de 8 µs, el cuanto se mantiene durante menos de 28 µs desde entrar
en ese nivel. El margen protege la decisión del umbral; después el ancho ya
es suficiente y no necesita precisión para decidir el disparo. Así, un pin
fijo al nivel contrario, incluso TRIG como entrada con pull-up, no deja un
temporizador periódico para siempre. Las ranuras desarmadas no preparan nada;
las que ya suenan mantienen el cuanto por el tren, sin preparar otro disparo.

La escritura que entra en preparación arranca el cuanto aunque no haya DISPARO.
Si el pulso vuelve al nivel de reposo antes del mínimo, cesa la preparación y
el siguiente tick deja de rearmarse, salvo que otro tren, preparación o cola
siga activo. Un disparo válido enlaza con la fase de tren sin aplazar el tick
pendiente. Durante el tren se protege el sondeo de todos los flancos; la cola
protege la observación del último, que puede cerrar un ancho medido.

Los trenes solapados comparten ese temporizador, sin aplazar un cuanto ya
pendiente al disparar otro. Un tren nuevo dentro de la cola mantiene el cuanto
y renueva la cola al aplicar su último paso. Sin trenes ni preparación, el
callback deja de rearmarse cuando ahora alcanza o supera `fin_cola_ns`, en el tick del límite
o el primero posterior. El reset anula `fin_cola_ns` y cancela el cuanto junto
con los pasos; `finalize` lo libera como antes. No hay temporizador periódico
durante el resto del tiempo. El C3 tiene una sola CPU y no cambia.
Los tiempos y la constante usan tipos de 64 bits, también en Windows (E-045).

Las regresiones de `armado_logica.c` cubren la consulta de actividad: una
configuración armada aún no suena, desarmar no corta el tren actual, todas las
ranuras cuentan y el reset termina la actividad. También cubren la condición
pura del cuanto antes, en y después del fin de cola, la actividad con una cola
vencida, la suma saturada y la anulación de la cola por reset.
Las regresiones de preparación cubren ambas polaridades, ranura desarmada,
consulta pura, límite exacto de 28 µs y sus vecinos, retorno a reposo tras 5 µs,
cola aún vigente, enlace al tren al alcanzar el mínimo, desarmado y reset.
Incluyen `minimo_ns = UINT32_MAX`, margen mayor que 32 bits, GPIO48 y reloj
cercano a `INT64_MAX`.
`armado_gpio_modelo.c`, compilado para ambos GPIO por `armado_unitarias.py`, ejecuta init, MMIO,
callbacks, lectura IN, reset y finalize reales con servicios QEMU dobles.
Comprueba disparo a +1000 ns, rearme desde el ahora incluso con atención tardía,
dos trenes solapados, continuidad durante los 20 µs posteriores al último paso,
parada en el límite o el primer tick posterior, prolongación por otro tren
dentro de la cola, reset durante trenes o cola y liberación. Cubre ambos órdenes
de callbacks coincidentes y el último paso entre ticks: el plazo del cuanto
ya está pendiente a ≤1 µs al terminar. Incluye tiempos mayores que 32 bits y un
tren de espera cero. En ambos GPIO reales también comprueba preparación de
ambas polaridades desde MMIO, expiración exacta, pin alto soltado como entrada,
rechazo de 5 µs con y sin cola, disparo al mínimo sin hueco ni aplazamiento,
desarmado y reset durante preparación, con arranque posterior por escritura.
Conserva las comprobaciones previas de plazos acumulados y atención
tardía. Las regresiones de ambos GPIO fallaron antes del cambio por faltar el
temporizador; después pasaron con MSVC, junto con la lógica y el receptor reales.
Las regresiones de cola fallaron también en ambos GPIO antes de este ajuste,
por faltar el cuanto pendiente tras el último paso.

`pruebas/todas.sh` elimina el reintento y ejecuta, una vez cada una, la imagen
original y `pruebas/armado_ocupado.flash.bin` con el mismo verificador `armado`.
README documenta la variante ocupada y cómo regenerarla; esta entrega reutiliza
su imagen ya compilada. Verificación local con `PYTHONIOENCODING=utf-8` y MSVC:
`python -m unittest discover -s pruebas -p '*_unitarias.py'` descubre 152 pruebas,
143 correctas y 9 omitidas por arneses que exigen GCC/Clang. Pasan también
`bash -n pruebas/todas.sh` y `git diff --check`; todos los archivos de texto
modificados tienen finales LF.

Las nuevas regresiones de preparación fallaron antes del ajuste: la consulta
pura devolvía falso y ambos GPIO carecían de cuanto pendiente antes del disparo.
Después pasan con MSVC. La validación local no compila ni ejecuta QEMU: queda
para el revisor recompilar y repetir `armado_ocupado` y `armado_prueba` 20 veces
cada una, con la tolerancia original de ±3 µs. Los resultados de QEMU anteriores
corresponden al cuanto con cola, todavía sin preparación.
