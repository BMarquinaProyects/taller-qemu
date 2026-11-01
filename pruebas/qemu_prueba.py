"""
Pruebas de humo de taller-qemu: ejecuta un firmware y comprueba su salida (y las tramas del puente).

  python pruebas/qemu_prueba.py <qemu-system-xtensa> humo   <firmware.bin> <marca>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> puente <puente_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> entrada <entrada_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> pwm <pwm_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> analogico <analogico_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> i2c <i2c_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> armado <armado_prueba.flash.bin>

Sale con 0 si todo cuadra. Sin dependencias fuera de la biblioteca estándar.
"""
import re
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


def es_hola(trama: tuple, minima: int) -> bool:
    tipo, version, _, carga = trama
    return tipo == 1 and minima <= version <= 5 and carga == struct.pack("<I", version)


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
    print(f"tramas: {len(tramas)} (GPIO {len(gpio)}, SPI {sum(1 for t in tramas if t[0] == 3)})")
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
    """Exige HOLA v4/v5, una dirección, lectura del espejo y escritura exacta."""
    errores = []
    if not tramas or not es_hola(tramas[0], 4):
        errores.append(f"la primera trama no es HOLA versión 4/5: {tramas[:1]}")
    encontrados = re.findall(r"^ENCONTRADO[^\r\n]*", texto, re.MULTILINE)
    if encontrados != ["ENCONTRADO 0x3C"]:
        errores.append(f"escaneo I2C incorrecto: {encontrados}")
    for linea in ("LEIDO=60,61", "TALLER-OK i2c"):
        if not re.search(r"^" + re.escape(linea) + r"\r?$", texto, re.MULTILINE):
            errores.append(f"sin línea {linea} en la UART")
    if not any(tp == 5 and v in (4, 5) and c == b"\x00\x3c\x00\xaf"
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
    ok = all(r for _, r in pasos) and hola in ([2], [3], [4], [5])
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
    ok = all(r for _, r in pasos) and hola in (
        [(3, struct.pack("<I", 3))], [(4, struct.pack("<I", 4))],
        [(5, struct.pack("<I", 5))])
    print("pasos:", pasos, "HOLA:", hola)
    print("OK analogico" if ok else "FALLO analogico\n" + buf[0][-600:])
    return 0 if ok else 1


def comprobar_pwm(tramas: list, texto: str) -> list[str]:
    """Exige versión 3/4/5 y una muestra del servo: 50 Hz, ciclo del 5 %."""
    errores = []
    if "TALLER-OK pwm" not in texto:
        errores.append("sin marca TALLER-OK pwm en la UART")
    if not tramas or not es_hola(tramas[0], 3):
        errores.append(f"la primera trama no es HOLA versión 3/4/5: {tramas[:1]}")
    coincide = False
    for tipo, version, _, carga in tramas:
        if tipo != 4:
            continue
        if version not in (3, 4, 5) or len(carga) != 9:
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
        errores.append(f"la primera trama no es HOLA versión 5: {tramas[:1]}")
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
    if (len(disparos) != 3 or any(v != 5 or len(c) != 1 for v, c in disparos) or
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


if __name__ == "__main__":
    qemu, modo = sys.argv[1], sys.argv[2]
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
    sys.exit(puente(qemu, sys.argv[3]))
