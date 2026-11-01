"""
Pruebas de humo de taller-qemu: ejecuta un firmware y comprueba su salida (y las tramas del puente).

  python pruebas/qemu_prueba.py <qemu-system-xtensa> humo   <firmware.bin> <marca>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> puente <puente_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> entrada <entrada_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> pwm <pwm_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> analogico <analogico_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> i2c <i2c_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> armado <armado_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> rmt <rmt_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> spi_dma <micropython.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_gpio <c3_gpio_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_pwm <c3_pwm_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_analogico <c3_analogico_prueba.flash.bin>

Sale con 0 si todo cuadra. Sin dependencias fuera de la biblioteca estándar.
"""
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

EFUSE_ECO3 = bytearray(124)
EFUSE_ECO3[13] = 0x80
EFUSE_ECO3[22] = 0x10
CABECERA = struct.Struct("<BBHQ")  # tipo, version, longitud_carga, t_ns
# Versión del protocolo del puente que compila este repo (TALLER_PUENTE_VERSION).
VERSION_PUENTE = 7
TIPO_TIEMPO = 8
LATIDO_NS = 10_000_000


def es_hola(trama: tuple, minima: int) -> bool:
    tipo, version, _, carga = trama
    return tipo == 1 and minima <= version <= VERSION_PUENTE and carga == struct.pack("<I", version)


def puerto_libre() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def conectar(puerto: int, proc: subprocess.Popen, espera_s: float = 15) -> socket.socket:
    fin = time.monotonic() + espera_s
    while time.monotonic() < fin:
        if proc.poll() is not None:
            sys.exit(f"QEMU terminó con código {proc.returncode}: {proc.stdout.read().decode(errors='replace')}")
        try:
            return socket.create_connection(("127.0.0.1", puerto), timeout=1)
        except OSError:
            time.sleep(0.1)
    sys.exit(f"QEMU no abrió el puerto {puerto}")


def leer_uart_hasta(sock: socket.socket, marcas: list[str], timeout_s: float) -> str:
    sock.settimeout(1)
    datos = b""
    fin = time.monotonic() + timeout_s
    while time.monotonic() < fin:
        try:
            trozo = sock.recv(4096)
        except socket.timeout:
            continue
        if not trozo:
            break
        datos += trozo
        texto = datos.decode(errors="replace")
        if any(m in texto for m in marcas):
            return texto
    return datos.decode(errors="replace")


def lanzar(qemu: str, firmware: str, con_puente: bool):
    tmp = tempfile.TemporaryDirectory()
    efuse = Path(tmp.name) / "efuse.bin"
    efuse.write_bytes(EFUSE_ECO3)
    p_uart = puerto_libre()
    cmd = [qemu, "-machine", "esp32", "-display", "none", "-monitor", "none",
           "-drive", f"file={firmware},if=mtd,format=raw",
           "-drive", f"file={efuse},if=none,format=raw,id=efuse",
           "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse",
           "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true"]
    p_puente = None
    if con_puente:
        p_puente = puerto_libre()
        cmd += ["-chardev", f"socket,id=taller-puente,host=127.0.0.1,port={p_puente},server=on,wait=on"]
    cmd += ["-serial", f"tcp:127.0.0.1:{p_uart},server=on,wait=on", "-icount", "shift=2,align=off,sleep=on"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return tmp, proc, p_uart, p_puente


def lanzar_c3(qemu: str, firmware: str):
    """C3: flash completa desde 0x0; eFuse v0.3 creada en RAM por Espressif."""
    tmp = tempfile.TemporaryDirectory()
    try:
        imagen = Path(tmp.name) / "c3.flash.bin"
        shutil.copyfile(firmware, imagen)
        if imagen.stat().st_size != 4 * 1024 * 1024:
            raise ValueError("C3 requiere una flash completa de 4 MiB")
        p_uart, p_puente = puerto_libre(), puerto_libre()
        cmd = [qemu, "-machine", "esp32c3", "-display", "none", "-monitor", "none",
               "-drive", f"file={imagen},if=mtd,format=raw",
               "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
               "-chardev", f"socket,id=taller-puente,host=127.0.0.1,port={p_puente},server=on,wait=on",
               "-serial", f"tcp:127.0.0.1:{p_uart},server=on,wait=on",
               "-icount", "shift=2,align=off,sleep=on"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except BaseException:
        tmp.cleanup()
        raise
    return tmp, proc, p_uart, p_puente


def comprobar_c3_gpio(tramas: list, texto: str) -> list[str]:
    """Exige HOLA v7, tres pulsos GPIO8 habilitados y lectura GPIO9 1→0."""
    errores = []
    if not tramas or not es_hola(tramas[0], 6):
        errores.append("sin HOLA v7")
    lineas = ("C3-ANTES=1", "C3-DESPUES=0", "TALLER-OK c3")
    posiciones = []
    for linea in lineas:
        coincidencias = list(re.finditer(r"^" + re.escape(linea) + r"\r?$", texto, re.MULTILINE))
        if len(coincidencias) != 1:
            errores.append(f"sin línea única {linea}")
        else:
            posiciones.append(coincidencias[0].start())
    if posiciones != sorted(posiciones) or "TALLER-FALLO" in texto:
        errores.append("lecturas UART fuera de orden o fallo del firmware")
    tiempos = [t for _, _, t, _ in tramas]
    if tiempos != sorted(tiempos):
        errores.append("t_ns decreciente")
    niveles = []
    ultimo_habilitado = False
    for tipo, version, _, carga in tramas:
        if version != VERSION_PUENTE:
            errores.append(f"trama con versión distinta de {VERSION_PUENTE}")
        if tipo != 2:
            continue
        if len(carga) != 16:
            errores.append("trama GPIO con longitud distinta de 16")
            continue
        out, enable = struct.unpack("<QQ", carga)
        if (out | enable) >> 22:
            errores.append("GPIO C3 fuera de 0..21")
        ultimo_habilitado = bool(enable & (1 << 8))
        if ultimo_habilitado:
            nivel = int(bool(out & (1 << 8)))
            if not niveles or niveles[-1] != nivel:
                niveles.append(nivel)
        elif niveles:
            errores.append("GPIO8 dejó de ser salida durante la prueba")
    if niveles != [0, 1, 0, 1, 0, 1, 0] or not ultimo_habilitado:
        errores.append(f"GPIO8: se esperaban tres pulsos completos, recibidos {niveles}")
    return errores


def c3_gpio(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar_c3(qemu, firmware)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, ["C3-ANTES=1", "TALLER-FALLO"], 60)
        if re.search(r"^C3-ANTES=1\r?$", texto, re.MULTILINE):
            # ENTRADA v6; solo se inyecta después de observar la lectura inicial.
            s_puente.sendall(CABECERA.pack(16, 6, 2, 0) + bytes([9, 0]))
            texto += leer_uart_hasta(s_uart, ["TALLER-OK c3", "TALLER-FALLO"], 30)
    finally:
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        parar.set()
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        if proc.stdout is not None:
            proc.stdout.close()
        tmp.cleanup()
    errores = comprobar_c3_gpio(tramas, texto)
    for error in errores:
        print("FALLO", error)
    print("FALLO c3_gpio\n" + texto[-1500:] if errores else
          "OK c3_gpio: GPIO8 tres pulsos; GPIO9 INPUT_PULLUP 1→0; HOLA v7")
    return 1 if errores else 0


def humo(qemu: str, firmware: str, marca: str) -> int:
    tmp, proc, p_uart, _ = lanzar(qemu, firmware, con_puente=False)
    try:
        texto = leer_uart_hasta(conectar(p_uart, proc), [marca, "TALLER-FALLO"], 60)
    finally:
        proc.kill()
        proc.wait()
        tmp.cleanup()
    ok = marca in texto
    print(("OK " if ok else "FALLO ") + marca, "\n" + texto[-600:] if not ok else "")
    return 0 if ok else 1


def leer_tramas(sock: socket.socket, tramas: list, parar: threading.Event) -> None:
    sock.settimeout(1)
    buf = b""
    while not parar.is_set():
        try:
            trozo = sock.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            break
        if not trozo:
            break
        buf += trozo
        while len(buf) >= CABECERA.size:
            tipo, version, largo, t_ns = CABECERA.unpack_from(buf)
            if len(buf) < CABECERA.size + largo:
                break
            carga = buf[CABECERA.size:CABECERA.size + largo]
            buf = buf[CABECERA.size + largo:]
            tramas.append((tipo, version, t_ns, carga))


def puente(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    try:
        s_puente = conectar(p_puente, proc)          # QEMU espera primero al puente…
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        texto = leer_uart_hasta(conectar(p_uart, proc), ["TALLER-OK puente"], 60)  # …y luego a la UART
        time.sleep(0.5)
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        tmp.cleanup()

    errores = []
    if "TALLER-OK puente" not in texto:
        errores.append("sin marca TALLER-OK puente en la UART")
    if not tramas or not es_hola(tramas[0], 1):
        errores.append(f"la primera trama no es HOLA: {tramas[:1]}")
    tiempos = [t for _, _, t, _ in tramas]
    if tiempos != sorted(tiempos):
        errores.append("t_ns decreciente")
    gpio = [struct.unpack("<QQ", c) for tp, _, _, c in tramas if tp == 2]
    b2, b33 = 1 << 2, 1 << 33
    if not any(n & b2 and n & b33 and s & b2 and s & b33 for n, s in gpio):
        errores.append("ninguna trama GPIO con los pines 2 y 33 en alto como salida")
    if not gpio or gpio[-1][0] & b2:
        errores.append("la última trama GPIO no tiene el pin 2 en bajo")
    spi = b"".join(c[1:] for tp, _, _, c in tramas if tp == 3 and c[0] == 3)
    if not spi.startswith(bytes([0xA5, 0x5A, 0x00, 0xFF])) or len(spi) != 1004:
        errores.append(f"SPI del bus 3: {len(spi)} bytes, empieza por {spi[:4].hex()}")
    else:
        esperado = bytes([0xA5, 0x5A, 0x00, 0xFF]) + bytes(i & 0xFF for i in range(1000))
        if spi != esperado:
            errores.append("los bytes SPI no coinciden con los enviados")
    # TIEMPO (v7): latidos sin carga cada 10 ms virtuales (con icount, un disparo puede llegar
    # unos µs tarde; la cadencia es fija y no acumula el retraso).
    latidos = [t for tp, _, t, c in tramas if tp == TIPO_TIEMPO and not c]
    # En Windows el anfitrión puede retrasar un disparo: un hueco largo va seguido de uno corto y
    # la media no cambia. Lo que importa al receptor: el tiempo avanza siempre y con cadencia fija.
    huecos = [b - a for a, b in zip(latidos, latidos[1:])]
    media = (latidos[-1] - latidos[0]) / len(huecos) if huecos else 0
    raros = [h for h in huecos if not 0.9 * LATIDO_NS <= h <= 1.1 * LATIDO_NS]
    if (len(latidos) < 3 or not all(0 < h <= 5 * LATIDO_NS for h in huecos)
            or not 0.9 * LATIDO_NS <= media <= 1.1 * LATIDO_NS):
        errores.append(f"latidos TIEMPO: {len(latidos)}, media {media:.0f} ns, fuera de ±10 %: {raros[:5]}")
    elif raros:
        print(f"aviso: {len(raros)} latidos fuera de ±10 % (media {media:.0f} ns): {raros[:5]}")
    print(f"tramas: {len(tramas)} (GPIO {len(gpio)}, SPI {sum(1 for t in tramas if t[0] == 3)}, "
          f"TIEMPO {len(latidos)})")
    for e in errores:
        print("FALLO", e)
    if not errores:
        print("OK puente")
    return 1 if errores else 0


TIPO_ENTRADA = 16
TIPO_ANALOGICO = 17


def trama_entrada(pin: int, nivel: int) -> bytes:
    return CABECERA.pack(TIPO_ENTRADA, 3, 2, 0) + bytes([pin, nivel])


def trama_analogico(gpio: int, valor: int) -> bytes:
    return CABECERA.pack(TIPO_ANALOGICO, 3, 3, 0) + struct.pack("<BH", gpio, valor)


def trama_i2c_dispositivo(bus: int, direccion: int, activo: bool) -> bytes:
    return CABECERA.pack(18, 4, 3, 0) + bytes([bus, direccion, int(activo)])


def trama_i2c_registros(bus: int, direccion: int, registro: int, datos: bytes) -> bytes:
    if len(datos) > 240:
        raise ValueError("I2C_REGISTROS admite hasta 240 bytes por trama")
    carga = bytes([bus, direccion, registro]) + datos
    return CABECERA.pack(19, 4, len(carga), 0) + carga


def comprobar_i2c(tramas: list, texto: str) -> list[str]:
    """Exige HOLA v4/v5/v6, una dirección, lectura del espejo y escritura exacta."""
    errores = []
    if not tramas or not es_hola(tramas[0], 4):
        errores.append(f"la primera trama no es HOLA versión 4/5/6: {tramas[:1]}")
    encontrados = re.findall(r"^ENCONTRADO[^\r\n]*", texto, re.MULTILINE)
    if encontrados != ["ENCONTRADO 0x3C"]:
        errores.append(f"escaneo I2C incorrecto: {encontrados}")
    for linea in ("LEIDO=60,61", "TALLER-OK i2c"):
        if not re.search(r"^" + re.escape(linea) + r"\r?$", texto, re.MULTILINE):
            errores.append(f"sin línea {linea} en la UART")
    if not any(tp == 5 and 4 <= v <= VERSION_PUENTE and c == b"\x00\x3c\x00\xaf"
               for tp, v, _, c in tramas):
        errores.append("sin I2C_ESCRITURA(0, 0x3C, 00 AF)")
    return errores


def i2c(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    s_puente = s_uart = hilo = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        # La conexión UART deja avanzar la inicialización de la máquina y HOLA.
        s_uart = conectar(p_uart, proc)
        fin = time.monotonic() + 15
        while not tramas and time.monotonic() < fin and proc.poll() is None:
            time.sleep(0.01)
        if tramas and es_hola(tramas[0], 4):
            s_puente.sendall(trama_i2c_dispositivo(0, 0x3c, True) +
                             trama_i2c_registros(0, 0x3c, 0xd0, b"\x60\x61"))
            texto = leer_uart_hasta(s_uart, ["TALLER-OK i2c", "TALLER-FALLO"], 60)
            time.sleep(0.5)
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        tmp.cleanup()

    errores = comprobar_i2c(tramas, texto)
    for error in errores:
        print("FALLO", error)
    print("FALLO i2c\n" + texto[-1000:] if errores else "OK i2c")
    return 1 if errores else 0


def esperar_analogico(sock: socket.socket, buf: list, minimo: int, maximo: int,
                     timeout_s: float) -> bool:
    """Espera una línea A=<valor> completa y nueva, dentro del intervalo inclusivo."""
    inicio = len(buf[0])
    fin = time.monotonic() + timeout_s
    sock.settimeout(0.2)
    while time.monotonic() < fin:
        try:
            trozo = sock.recv(4096)
        except socket.timeout:
            continue
        if not trozo:
            return False
        buf[0] += trozo.decode(errors="replace")
        for muestra in re.finditer(r"^A=([0-9]+)\r?\n", buf[0], re.MULTILINE):
            if muestra.start() >= inicio and minimo <= int(muestra[1]) <= maximo:
                return True
    return False


def esperar_en_uart(sock: socket.socket, buf: list, texto: str, timeout_s: float) -> bool:
    """Lee la UART acumulando en buf[0] hasta que aparezca `texto` después de la posición inicial."""
    inicio = len(buf[0])
    fin = time.monotonic() + timeout_s
    sock.settimeout(0.2)
    while time.monotonic() < fin:
        try:
            trozo = sock.recv(4096)
        except socket.timeout:
            continue
        if not trozo:
            return False
        buf[0] += trozo.decode(errors="replace")
        if texto in buf[0][inicio:]:
            return True
    return False


def entrada(qemu: str, firmware: str) -> int:
    """El programa externo cambia GPIO15 por el puente y el sketch lo lee (v2..v5)."""
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    buf = [""]
    pasos = []
    try:
        s_puente = conectar(p_puente, proc)
        threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True).start()
        s_uart = conectar(p_uart, proc)
        pasos.append(("suelto=1", esperar_en_uart(s_uart, buf, "NIVEL=1", 30)))
        s_puente.sendall(trama_entrada(15, 0))
        pasos.append(("pulsado=0", esperar_en_uart(s_uart, buf, "NIVEL=0", 5)))
        s_puente.sendall(trama_entrada(15, 1))
        pasos.append(("soltado=1", esperar_en_uart(s_uart, buf, "NIVEL=1", 5)))
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        tmp.cleanup()
    hola = [struct.unpack("<I", c)[0] for tp, _, _, c in tramas if tp == 1]
    ok = all(r for _, r in pasos) and len(hola) == 1 and 2 <= hola[0] <= VERSION_PUENTE
    print("pasos:", pasos, "HOLA:", hola)
    print("OK entrada" if ok else "FALLO entrada\n" + buf[0][-400:])
    return 0 if ok else 1


def analogico(qemu: str, firmware: str) -> int:
    """Inyecta valores crudos en GPIO34 (ADC1 canal 6) mediante el puente v3."""
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    buf = [""]
    pasos = []
    s_puente = s_uart = hilo = None
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        pasos.append(("inicial=0", esperar_analogico(s_uart, buf, 0, 0, 30)))
        s_puente.sendall(trama_analogico(34, 2048))
        pasos.append(("medio=1900..2200", esperar_analogico(s_uart, buf, 1900, 2200, 5)))
        s_puente.sendall(trama_analogico(34, 4095))
        pasos.append(("alto=4000..4095", esperar_analogico(s_uart, buf, 4000, 4095, 5)))
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        tmp.cleanup()

    hola = [(v, c) for tp, v, _, c in tramas if tp == 1]
    ok = all(r for _, r in pasos) and len(hola) == 1 and 3 <= hola[0][0] <= VERSION_PUENTE \
        and hola[0][1] == struct.pack("<I", hola[0][0])
    print("pasos:", pasos, "HOLA:", hola)
    print("OK analogico" if ok else "FALLO analogico\n" + buf[0][-600:])
    return 0 if ok else 1


def comprobar_pwm(tramas: list, texto: str) -> list[str]:
    """Exige versión 3/4/5/6 y una muestra del servo: 50 Hz, ciclo del 5 %."""
    errores = []
    if "TALLER-OK pwm" not in texto:
        errores.append("sin marca TALLER-OK pwm en la UART")
    if not tramas or not es_hola(tramas[0], 3):
        errores.append(f"la primera trama no es HOLA versión 3/4/5: {tramas[:1]}")
    coincide = False
    for tipo, version, _, carga in tramas:
        if tipo != 4:
            continue
        if not 3 <= version <= VERSION_PUENTE or len(carga) != 9:
            errores.append(f"trama PWM inválida: versión {version}, {len(carga)} bytes")
            continue
        gpio, frecuencia_mhz, ciclo_ppm = struct.unpack("<BII", carga)
        if gpio == 13 and 49500 <= frecuencia_mhz <= 50500 and 47000 <= ciclo_ppm <= 53000:
            coincide = True
    if not coincide:
        errores.append("sin PWM en GPIO13 de 50 Hz ±1 % y ciclo 5 % ±0,3 %")
    return errores


def pwm(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    s_puente = s_uart = hilo = None
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, ["TALLER-OK pwm", "TALLER-FALLO"], 60)
        time.sleep(0.5)  # Dejar que el lector del puente recoja las últimas tramas.
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        tmp.cleanup()

    errores = comprobar_pwm(tramas, texto)
    print(f"tramas: {len(tramas)} (PWM {sum(1 for t in tramas if t[0] == 4)})")
    for error in errores:
        print("FALLO", error)
    if errores:
        print(texto[-600:])
    else:
        print("OK pwm")
    return 1 if errores else 0


def trama_entrada_armada(pin_disparo: int, flanco: int, minimo_ns: int,
                         pin_salida: int, pasos: list[tuple[int, int]]) -> bytes:
    if (not 0 <= pin_disparo < 40 or flanco not in (0, 1) or
            not 0 <= minimo_ns <= 0xffffffff or not 0 <= pin_salida < 40 or
            len(pasos) > 200 or any(not 0 <= espera <= 0xffffffff or nivel not in (0, 1)
                                    for espera, nivel in pasos)):
        raise ValueError("ENTRADA_ARMADA inválida: GPIO 0..39, flanco/nivel 0..1, u32 y n<=200")
    carga = struct.pack("<BBIBH", pin_disparo, flanco, minimo_ns, pin_salida, len(pasos))
    carga += b"".join(struct.pack("<IB", espera, nivel) for espera, nivel in pasos)
    return CABECERA.pack(20, 5, len(carga), 0) + carga


def comprobar_armado(tramas: list, texto: str) -> list[str]:
    """Medidas completas ±3 µs y exactamente dos DISPARO(4) y un DISPARO(5)."""
    errores = []
    if not tramas or not es_hola(tramas[0], 5):
        errores.append(f"la primera trama no es HOLA versión 5/6: {tramas[:1]}")
    for etiqueta in ("DHT", "DHT2"):
        lineas = re.findall(r"^" + etiqueta + r"=[^\r\n]*", texto, re.MULTILINE)
        medida = re.fullmatch(etiqueta + r"=(\d+),(\d+),(\d+),(\d+)", lineas[0]) if len(lineas) == 1 else None
        if medida is None or any(abs(int(v) - esperado) > 3
                                 for v, esperado in zip(medida.groups(), (80, 80, 50, 70))):
            errores.append(f"{etiqueta} fuera de ±3 µs o incompleto: {lineas}")
    ecos = re.findall(r"^ECO=[^\r\n]*", texto, re.MULTILINE)
    eco = re.fullmatch(r"ECO=(\d+)", ecos[0]) if len(ecos) == 1 else None
    if eco is None or not 1163 <= int(eco[1]) <= 1169:
        errores.append(f"ECO fuera de 1163..1169 µs: {ecos}")
    for linea in ("CORTO=SIN_RESPUESTA", "ECO_CORTO=0", "TALLER-OK armado"):
        if len(re.findall(r"^" + re.escape(linea) + r"\r?$", texto, re.MULTILINE)) != 1:
            errores.append(f"sin línea única {linea}")
    if "TALLER-FALLO" in texto:
        errores.append("el firmware anunció TALLER-FALLO")
    disparos = [(v, c) for tp, v, _, c in tramas if tp == 6]
    if (len(disparos) != 3 or any(not 5 <= v <= VERSION_PUENTE or len(c) != 1 for v, c in disparos) or
            [c for _, c in disparos].count(b"\x04") != 2 or
            [c for _, c in disparos].count(b"\x05") != 1):
        errores.append(f"DISPARO incorrectos (esperados pin 4: 2, pin 5: 1): {disparos}")
    tiempos = [t for _, _, t, _ in tramas]
    if tiempos != sorted(tiempos):
        errores.append("t_ns decreciente")
    return errores


def armado(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    s_puente = s_uart = hilo = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        fin = time.monotonic() + 15
        while not tramas and time.monotonic() < fin and proc.poll() is None:
            time.sleep(0.01)
        if tramas and es_hola(tramas[0], 5):
            # TRIG debe estar ya bajo antes de armar: habilitar OUTPUT bajo
            # desde INPUT (conducido alto) también produce un flanco válido.
            texto = leer_uart_hasta(s_uart, ["ARMADO-LISTO"], 30)
            if "ARMADO-LISTO" not in texto:
                print("FALLO armado: sin ARMADO-LISTO\n" + texto[-1000:])
                return 1
            # El sketch espera 1 s antes de la primera lectura. ECHO reposa bajo.
            s_puente.sendall(trama_entrada(18, 0) +
                             trama_entrada_armada(4, 1, 800000, 4,
                                 [(30000, 0), (80000, 1), (80000, 0),
                                  (50000, 1), (70000, 0), (50000, 1)]) +
                             trama_entrada_armada(5, 0, 8000, 18,
                                 [(450000, 1), (1166000, 0)]))
            texto += leer_uart_hasta(s_uart, ["TALLER-OK armado", "TALLER-FALLO"], 60)
            time.sleep(0.5)
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        tmp.cleanup()

    errores = comprobar_armado(tramas, texto)
    print("DISPARO:", {pin: sum(tp == 6 and c == bytes([pin])
                                 for tp, _, _, c in tramas) for pin in (4, 5)})
    for error in errores:
        print("FALLO", error)
    print("FALLO armado\n" + texto[-1000:] if errores else "OK armado")
    return 1 if errores else 0


def decodificar_rmt(simbolos: list[int], tick_ps: int) -> list[tuple[int, int, int]]:
    """WS2812: pulsos altos >0,55 µs son 1; bytes GRB, MSB primero."""
    if tick_ps <= 0:
        raise ValueError("tick_ps debe ser positivo")
    bits = []
    alto = 0
    terminado = False
    for indice, simbolo in enumerate(simbolos):
        for mitad in (0, 16):
            duracion = (simbolo >> mitad) & 0x7fff
            nivel = (simbolo >> (mitad + 15)) & 1
            if duracion == 0:
                if indice != len(simbolos) - 1:
                    raise ValueError("símbolos después del fin")
                terminado = True
                break
            if nivel:
                if terminado:
                    raise ValueError("datos después del reset WS2812")
                alto += duracion * tick_ps
            else:
                if alto:
                    bits.append(int(alto > 550000))
                    alto = 0
                if duracion * tick_ps > 50000000:
                    terminado = True
    if alto or len(bits) % 24:
        raise ValueError(f"tren WS2812 incompleto: {len(bits)} bits")
    datos = []
    for inicio in range(0, len(bits), 8):
        valor = 0
        for bit in bits[inicio:inicio + 8]:
            valor = (valor << 1) | bit
        datos.append(valor)
    return [(datos[i + 1], datos[i], datos[i + 2]) for i in range(0, len(datos), 3)]


def comprobar_rmt(tramas: list, texto: str) -> list[str]:
    errores = []
    if not tramas or not es_hola(tramas[0], 6):
        errores.append("sin HOLA versión 6")
    medidas = re.findall(r"^T_SHOW=[^\r\n]*", texto, re.MULTILINE)
    medida = re.fullmatch(r"T_SHOW=(\d+)", medidas[0]) if len(medidas) == 1 else None
    if medida is None or not 200 <= int(medida[1]) <= 2000:
        errores.append(f"T_SHOW fuera de 200..2000 µs o incompleto: {medidas}")
    if (len(re.findall(r"^TALLER-OK rmt\r?$", texto, re.MULTILINE)) != 1 or
            "TALLER-FALLO" in texto):
        errores.append("sin marca única TALLER-OK rmt o fallo del firmware")
    trenes = []
    anterior = None
    for tipo, version, t_ns, carga in tramas:
        if tipo != 7:
            anterior = None
            continue
        if version != VERSION_PUENTE or len(carga) < 10 or len(carga) > 65535 or (len(carga) - 6) % 4:
            errores.append("trama RMT inválida")
            anterior = None
            continue
        canal, gpio, tick_ps = struct.unpack_from("<BBI", carga)
        if canal > 7 or gpio != 13 or tick_ps == 0:
            errores.append(f"RMT: canal={canal}, gpio={gpio}, tick_ps={tick_ps}")
            continue
        clave = (t_ns, canal, gpio, tick_ps)
        simbolos = list(struct.unpack_from(f"<{(len(carga) - 6) // 4}I", carga, 6))
        if clave == anterior:
            trenes[-1][1].extend(simbolos)
        else:
            trenes.append((clave, simbolos))
        anterior = clave
    esperados = [[(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
                  (0, 0, 0), (1, 2, 3), (128, 64, 32), (255, 255, 0)],
                 [(i * 7, 255 - i * 5, i * 3) for i in range(30)]]
    if len(trenes) != 2:
        errores.append(f"se esperaban dos transmisiones RMT, recibidas {len(trenes)}")
    fin_anterior = 0
    for i, (clave, simbolos) in enumerate(trenes):
        if clave[0] < fin_anterior:
            errores.append("tiempos RMT decrecientes o transmisiones solapadas")
        ticks = 0
        for simbolo in simbolos:
            d0, d1 = simbolo & 0x7fff, (simbolo >> 16) & 0x7fff
            if not d0:
                break
            ticks += d0 + d1
            if not d1:
                break
        fin_anterior = clave[0] + (ticks * clave[3] + 999) // 1000
        try:
            colores = decodificar_rmt(simbolos, clave[3])
            if i >= 2 or colores != esperados[i]:
                errores.append(f"colores RMT del tren {i} incorrectos: {colores}")
        except ValueError as error:
            errores.append(str(error))
    return errores


def rmt(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, con_puente=True)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, ["TALLER-OK rmt", "TALLER-FALLO"], 60)
        time.sleep(0.5)
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        tmp.cleanup()
    errores = comprobar_rmt(tramas, texto)
    for error in errores:
        print("FALLO", error)
    print("FALLO rmt\n" + texto[-1000:] if errores else "OK rmt: 8 y 30 LED completos; " +
          ", ".join(re.findall(r"^T_SHOW=\d+", texto, re.MULTILINE)) + " µs (200..2000)")
    return 1 if errores else 0


def comprobar_spi_dma(tramas: list, texto: str) -> list[str]:
    """Exige todo MOSI VSPI, incluidos los ceros de read(4), y su resultado RX."""
    errores = []
    if not tramas or not es_hola(tramas[0], 6):
        errores.append("sin HOLA v7")
    for linea in ("MISO=ffffffff", "TALLER-OK spi_dma"):
        if len(re.findall(r"^" + re.escape(linea) + r"\r?$", texto, re.MULTILINE)) != 1:
            errores.append(f"sin línea única {linea}")
    if "Traceback" in texto or "TALLER-FALLO" in texto:
        errores.append("MicroPython anunció un fallo")
    spi = []
    tiempos = []
    for tipo, version, t_ns, carga in tramas:
        if tipo != 3:
            continue
        if version != VERSION_PUENTE or len(carga) < 2 or carga[0] != 3:
            errores.append("trama SPI inválida o fuera de VSPI")
            continue
        spi.append(carga[1:])
        tiempos.append(t_ns)
    esperado = b"abcd" + bytes(range(256)) * 160 + b"\0" * 4
    datos = b"".join(spi)
    if datos != esperado:
        errores.append(f"MOSI VSPI distinto: {len(datos)} bytes, esperados {len(esperado)}")
    if tiempos != sorted(tiempos):
        errores.append("tiempos SPI decrecientes")
    return errores


def ejecutar_raw_paste(sock: socket.socket, codigo: str, timeout_s: float = 60) -> str:
    """REPL raw paste con crédito por ventana, EOF comprobado y plazo total."""
    fin = time.monotonic() + timeout_s

    def leer(n):
        datos = bytearray()
        while len(datos) < n:
            restante = fin - time.monotonic()
            if restante <= 0:
                raise TimeoutError("plazo agotado en REPL raw paste")
            sock.settimeout(restante)
            trozo = sock.recv(n - len(datos))
            if not trozo:
                raise EOFError("UART cerrada durante REPL raw paste")
            datos.extend(trozo)
        return bytes(datos)

    def hasta(marca):
        datos = bytearray()
        while not datos.endswith(marca):
            datos.extend(leer(1))
        return bytes(datos[:-len(marca)])

    sock.sendall(b"\r\x03\x03\x01")
    hasta(b"raw REPL; CTRL-B to exit\r\n>")
    sock.sendall(b"\x05A\x01")
    if leer(2) != b"R\x01":
        raise ValueError("MicroPython no admite raw paste")
    ventana = int.from_bytes(leer(2), "little")
    if not ventana:
        raise ValueError("ventana raw paste nula")
    datos = codigo.encode("utf-8")
    credito, pos = ventana, 0
    while pos < len(datos):
        if credito == 0:
            if leer(1) != b"\x01":
                raise ValueError("MicroPython interrumpió raw paste")
            credito += ventana
        n = min(credito, len(datos) - pos)
        sock.sendall(datos[pos:pos + n])
        pos += n
        credito -= n
    sock.sendall(b"\x04")
    while True:
        control = leer(1)
        if control == b"\x04":
            break
        if control != b"\x01":
            raise ValueError(f"control raw paste inesperado: {control!r}")
    salida = hasta(b"\x04")
    errores = hasta(b"\x04")
    hasta(b">")
    if errores:
        raise RuntimeError(errores.decode(errors="replace"))
    return salida.decode(errors="replace")


def spi_dma(qemu: str, firmware: str) -> int:
    # MicroPython puede crear su VFS. Cada prueba trabaja con una copia privada.
    with tempfile.TemporaryDirectory() as directorio:
        imagen = Path(directorio) / "micropython.flash.bin"
        shutil.copyfile(firmware, imagen)
        if imagen.stat().st_size != 4 * 1024 * 1024:
            raise ValueError("SPI DMA requiere una imagen completa de flash de 4 MiB")
        tmp, proc, p_uart, p_puente = lanzar(qemu, str(imagen), con_puente=True)
        tramas: list = []
        parar = threading.Event()
        s_puente = s_uart = hilo = None
        texto = ""
        try:
            s_puente = conectar(p_puente, proc)
            hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
            hilo.start()
            s_uart = conectar(p_uart, proc)
            arranque = leer_uart_hasta(s_uart, [">>> "], 60)
            if ">>> " not in arranque:
                raise TimeoutError("sin REPL MicroPython: " + arranque[-1000:])
            programa = Path(__file__).with_name("spi_dma_micropython.py").read_text(encoding="utf-8")
            texto = ejecutar_raw_paste(s_uart, programa)
            # La UART y el puente son sockets distintos: drenar el último SPI.
            fin = time.monotonic() + 2
            while time.monotonic() < fin:
                if sum(len(c) - 1 for tp, _, _, c in tramas if tp == 3 and c) >= 40968:
                    break
                time.sleep(0.01)
        except (OSError, EOFError, ValueError, RuntimeError) as exc:
            texto += "\nTALLER-FALLO " + str(exc)
        finally:
            proc.kill()
            proc.wait()
            if hilo is not None:
                hilo.join(timeout=2)
            parar.set()
            for conexion in (s_puente, s_uart):
                if conexion is not None:
                    conexion.close()
            tmp.cleanup()
        errores = comprobar_spi_dma(tramas, texto)
        for error in errores:
            print("FALLO", error)
        print("FALLO spi_dma\n" + texto[-1500:] if errores else
              "OK spi_dma: abcd, 40960 bytes exactos y MISO=ffffffff")
        return 1 if errores else 0


# --- ESP32-C3: LEDC (PWM) y ADC1 -------------------------------------------------------------

# GPIO8: servo al 5 %, 25 %, cambio a 100 Hz y apagado (frecuencia en mHz, ciclo en ppm).
PASOS_C3_PWM = ((50000, 49987), (50000, 250000), (100000, 250000), (100000, 0))


def linea_unica(texto: str, linea: str) -> bool:
    """La línea completa (con su salto) aparece exactamente una vez."""
    return len(re.findall(r"^" + re.escape(linea) + r"\r?\n", texto, re.MULTILINE)) == 1


def comprobar_c3_pwm(tramas: list, texto: str) -> list[str]:
    """HOLA v7, tramas PWM válidas del C3 y la secuencia de GPIO8 en orden, con OK único."""
    errores = []
    if not tramas or not es_hola(tramas[0], VERSION_PUENTE):
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
    if not linea_unica(texto, "TALLER-OK c3_pwm") or "TALLER-FALLO" in texto:
        errores.append("sin línea única TALLER-OK c3_pwm o fallo del firmware")
    vistos = []
    for tipo, version, _, carga in tramas:
        if tipo != 4:
            continue
        if version != VERSION_PUENTE or len(carga) != 9:
            errores.append(f"trama PWM inválida: versión {version}, {len(carga)} bytes")
            continue
        gpio, frecuencia, ciclo = struct.unpack("<BII", carga)
        if gpio > 21 or ciclo > 1_000_000 or (frecuencia == 0 and ciclo):
            errores.append(f"trama PWM imposible en el C3: GPIO{gpio}, {frecuencia} mHz, {ciclo} ppm")
            continue
        if gpio == 8 and (not vistos or vistos[-1] != (frecuencia, ciclo)):
            vistos.append((frecuencia, ciclo))
    # Cada paso, dentro de un 0,5 % (el divisor del LEDC es de punto fijo), y en orden.
    i = 0
    for frecuencia, ciclo in vistos:
        if i < len(PASOS_C3_PWM):
            f, c = PASOS_C3_PWM[i]
            if abs(frecuencia - f) <= f // 200 and abs(ciclo - c) <= max(c // 200, 0):
                i += 1
    if i != len(PASOS_C3_PWM):
        errores.append(f"GPIO8: secuencia PWM incompleta o desordenada: {vistos}")
    return errores


def c3_pwm(qemu: str, firmware: str) -> int:
    tmp, proc, p_uart, p_puente = lanzar_c3(qemu, firmware)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, ["TALLER-OK c3_pwm", "TALLER-FALLO"], 60)
        # La marca llega antes que su salto de línea: leer un poco más (y dejar que el lector
        # del puente recoja las últimas tramas).
        texto += leer_uart_hasta(s_uart, ["\n"], 0.5) if not texto.endswith("\n") else ""
        time.sleep(0.5)
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        if proc.stdout is not None:
            proc.stdout.close()
        tmp.cleanup()
    errores = comprobar_c3_pwm(tramas, texto)
    print(f"tramas: {len(tramas)} (PWM {sum(1 for t in tramas if t[0] == 4)})")
    for error in errores:
        print("FALLO", error)
    print("FALLO c3_pwm\n" + texto[-800:] if errores else
          "OK c3_pwm: GPIO8 50 Hz 5 % → 25 % → 100 Hz → apagado")
    return 1 if errores else 0


def comprobar_c3_analogico(tramas: list, texto: str) -> list[str]:
    """HOLA v7 y lecturas de GPIO0 completas: 0, después 1900..2200 y después 4000..4095."""
    errores = []
    if not tramas or not es_hola(tramas[0], VERSION_PUENTE):
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
    if not linea_unica(texto, "TALLER-OK c3_analogico") or "TALLER-FALLO" in texto:
        errores.append("sin línea única TALLER-OK c3_analogico o fallo del firmware")
    lecturas = []
    for linea in texto.splitlines():
        if "C3-A=" not in linea:
            continue
        m = re.fullmatch(r"C3-A=([0-9]+)", linea.strip("\r"))
        if not m:
            errores.append(f"línea de lectura rota: {linea!r}")
            continue
        valor = int(m[1])
        if valor > 4095:
            errores.append(f"lectura fuera de 12 bits: {valor}")
        lecturas.append(valor)
    fases = [lambda v: v == 0, lambda v: 1900 <= v <= 2200, lambda v: 4000 <= v <= 4095]
    if not lecturas or lecturas[0] != 0:
        errores.append(f"la primera lectura no es 0: {lecturas[:3]}")
    i = 0
    for v in lecturas:
        if i < len(fases) and fases[i](v):
            i += 1
    if i != len(fases):
        errores.append(f"lecturas sin la secuencia 0 → medio → alto: {lecturas[:12]}")
    return errores


def esperar_c3_analogico(sock: socket.socket, buf: list, minimo: int, maximo: int,
                         timeout_s: float) -> bool:
    """Como esperar_analogico, con líneas C3-A=<valor>."""
    inicio = len(buf[0])
    fin = time.monotonic() + timeout_s
    sock.settimeout(0.2)
    while time.monotonic() < fin:
        try:
            trozo = sock.recv(4096)
        except socket.timeout:
            continue
        if not trozo:
            return False
        buf[0] += trozo.decode(errors="replace")
        for muestra in re.finditer(r"^C3-A=([0-9]+)\r?\n", buf[0], re.MULTILINE):
            if muestra.start() >= inicio and minimo <= int(muestra[1]) <= maximo:
                return True
    return False


def c3_analogico(qemu: str, firmware: str) -> int:
    """Inyecta valores crudos en GPIO0 (ADC1 canal 0 del C3) con ANALOGICO."""
    tmp, proc, p_uart, p_puente = lanzar_c3(qemu, firmware)
    tramas: list = []
    parar = threading.Event()
    buf = [""]
    pasos = []
    s_puente = s_uart = hilo = None
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        pasos.append(("inicial=0", esperar_c3_analogico(s_uart, buf, 0, 0, 60)))
        s_puente.sendall(trama_analogico(0, 2048))
        pasos.append(("medio=1900..2200", esperar_c3_analogico(s_uart, buf, 1900, 2200, 10)))
        s_puente.sendall(trama_analogico(0, 4095))
        pasos.append(("alto=4000..4095", esperar_c3_analogico(s_uart, buf, 4000, 4095, 10)))
        if "TALLER-OK c3_analogico" not in buf[0]:
            buf[0] += leer_uart_hasta(s_uart, ["TALLER-OK c3_analogico", "TALLER-FALLO"], 10)
        if not buf[0].endswith("\n"):
            buf[0] += leer_uart_hasta(s_uart, ["\n"], 0.5)  # el salto de la última línea
    finally:
        parar.set()
        proc.kill()
        proc.wait()
        if hilo is not None:
            hilo.join(timeout=2)
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        if proc.stdout is not None:
            proc.stdout.close()
        tmp.cleanup()
    errores = comprobar_c3_analogico(tramas, buf[0])
    print("pasos:", pasos)
    for error in errores:
        print("FALLO", error)
    print("FALLO c3_analogico\n" + buf[0][-800:] if errores else
          "OK c3_analogico: GPIO0 0 → 2048 → 4095")
    return 1 if errores else 0


if __name__ == "__main__":
    qemu, modo = sys.argv[1], sys.argv[2]
    if modo == "c3_gpio":
        sys.exit(c3_gpio(qemu, sys.argv[3]))
    if modo == "c3_pwm":
        sys.exit(c3_pwm(qemu, sys.argv[3]))
    if modo == "c3_analogico":
        sys.exit(c3_analogico(qemu, sys.argv[3]))
    if modo == "humo":
        sys.exit(humo(qemu, sys.argv[3], sys.argv[4]))
    if modo == "entrada":
        sys.exit(entrada(qemu, sys.argv[3]))
    if modo == "pwm":
        sys.exit(pwm(qemu, sys.argv[3]))
    if modo == "analogico":
        sys.exit(analogico(qemu, sys.argv[3]))
    if modo == "i2c":
        sys.exit(i2c(qemu, sys.argv[3]))
    if modo == "armado":
        sys.exit(armado(qemu, sys.argv[3]))
    if modo == "rmt":
        sys.exit(rmt(qemu, sys.argv[3]))
    if modo == "spi_dma":
        sys.exit(spi_dma(qemu, sys.argv[3]))
    sys.exit(puente(qemu, sys.argv[3]))
