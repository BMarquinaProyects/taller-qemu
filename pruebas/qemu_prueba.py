"""
Pruebas de humo de taller-qemu: ejecuta un firmware y comprueba su salida (y las tramas del puente).

  python pruebas/qemu_prueba.py <qemu-system-xtensa> humo   <firmware.bin> <marca>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> puente <puente_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> entrada <entrada_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> pull <pull_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_pull <c3_pull_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_pull <s3_pull_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> pwm <pwm_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> analogico <analogico_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> i2c <i2c_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> armado <armado_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> rmt <rmt_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> fpu <fpu_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> spi_dma <micropython.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_gpio <c3_gpio_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_pwm <c3_pwm_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_analogico <c3_analogico_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_i2c <c3_i2c_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_rmt <c3_rmt_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-riscv32> c3_spi <c3_spi_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_gpio <s3_gpio_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_pwm <s3_pwm_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_analogico <s3_analogico_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_i2c <s3_i2c_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_rmt <s3_rmt_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_spi <s3_spi_prueba.flash.bin>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> s3_fpu <s3_fpu_prueba.flash.bin>

Sale con 0 si todo cuadra. Sin dependencias fuera de la biblioteca estándar.
"""
import os
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
VERSION_PUENTE = 8
TIPO_TIEMPO = 8
TIPO_PULL = 9
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


def lanzar_soc(qemu: str, firmware: str, chip: str, flash_mib: int = 4):
    """C3/S3: copia privada de flash de 4 MiB, bootloader en 0x0 y UART0.

    Con `flash_mib` mayor, la copia se rellena con 0xFF (flash borrada) hasta ese tamaño: QEMU elige el
    chip por el tamaño (16 MiB: IS25LP128, el único que acepta QIO)."""
    tmp = tempfile.TemporaryDirectory()
    try:
        imagen = Path(tmp.name) / f"{chip}.flash.bin"
        shutil.copyfile(firmware, imagen)
        if imagen.stat().st_size != 4 * 1024 * 1024:
            raise ValueError(f"{chip.upper()} requiere una flash completa de 4 MiB")
        if flash_mib > 4:
            with open(imagen, "ab") as f:
                f.write(b"\xff" * ((flash_mib - 4) * 1024 * 1024))
        p_uart, p_puente = puerto_libre(), puerto_libre()
        cmd = [qemu, "-machine", f"esp32{chip}", "-display", "none", "-monitor", "none",
               "-drive", f"file={imagen},if=mtd,format=raw",
               "-global", f"driver=timer.esp32{chip}.timg,property=wdt_disable,value=true",
               "-chardev", f"socket,id=taller-puente,host=127.0.0.1,port={p_puente},server=on,wait=on",
               "-serial", f"tcp:127.0.0.1:{p_uart},server=on,wait=on",
               "-icount", "shift=2,align=off,sleep=on"]
        # La traza ADC puede llenar la tubería que este lanzador no consume.
        # Heredar stderr permite verla o redirigirla sin detener el firmware.
        error = None if chip == "s3" and os.environ.get("TALLER_ADC_TRAZA") == "1" else subprocess.STDOUT
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=error)
    except BaseException:
        tmp.cleanup()
        raise
    return tmp, proc, p_uart, p_puente


def comprobar_gpio_soc(tramas: list, texto: str, chip: str, pin: int,
                        pines: int, mascara: int) -> list[str]:
    """Exige tres pulsos habilitados y lectura externa 1→0 en C3/S3."""
    errores = []
    if not tramas or not es_hola(tramas[0], VERSION_PUENTE):
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
    marca = "c3" if chip == "c3" else "s3_gpio"
    lineas = (f"{chip.upper()}-ANTES=1", f"{chip.upper()}-DESPUES=0", f"TALLER-OK {marca}")
    posiciones = []
    for linea in lineas:
        coincidencias = list(re.finditer(r"^" + re.escape(linea) + r"\r?\n", texto, re.MULTILINE))
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
        if (out | enable) & ~mascara:
            errores.append(f"GPIO {chip.upper()} fuera de sus {pines} índices válidos")
        ultimo_habilitado = bool(enable & (1 << pin))
        if ultimo_habilitado:
            nivel = int(bool(out & (1 << pin)))
            if not niveles or niveles[-1] != nivel:
                niveles.append(nivel)
        elif niveles:
            errores.append(f"GPIO{pin} dejó de ser salida durante la prueba")
    if niveles != [0, 1, 0, 1, 0, 1, 0] or not ultimo_habilitado:
        errores.append(f"GPIO{pin}: se esperaban tres pulsos completos, recibidos {niveles}")
    return errores


def gpio_soc(qemu: str, firmware: str, chip: str, salida: int, entrada: int) -> int:
    marca = "c3" if chip == "c3" else "s3_gpio"
    prefijo = chip.upper()
    tmp, proc, p_uart, p_puente = lanzar_soc(qemu, firmware, chip)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, [f"{prefijo}-ANTES=1", "TALLER-FALLO"], 60)
        if not texto.endswith("\n"):
            texto += leer_uart_hasta(s_uart, ["\n"], 0.5)
        if re.search(rf"^{prefijo}-ANTES=1\r?\n", texto, re.MULTILINE):
            # ENTRADA v6; solo se inyecta después de observar la lectura inicial.
            s_puente.sendall(CABECERA.pack(16, 6, 2, 0) + bytes([entrada, 0]))
            texto += leer_uart_hasta(s_uart, [f"TALLER-OK {marca}", "TALLER-FALLO"], 30)
        if not texto.endswith("\n"):
            texto += leer_uart_hasta(s_uart, ["\n"], 0.5)
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
    errores = comprobar_c3_gpio(tramas, texto) if chip == "c3" else comprobar_s3_gpio(tramas, texto)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {chip}_gpio\n" + texto[-1500:] if errores else
          f"OK {chip}_gpio: GPIO{salida} tres pulsos; GPIO{entrada} INPUT_PULLUP 1→0; HOLA v8")
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


def comprobar_fpu(texto: str) -> list[str]:
    """Exige CPENABLE=0xff y resultados FPU exactos, en líneas completas y ordenadas."""
    errores = []
    posiciones = []
    for linea in ("TALLER fpu cpenable=0xff", "TALLER-OK fpu 105 -21 3"):
        coincidencias = list(re.finditer(r"^" + re.escape(linea) + r"\r?\n", texto, re.MULTILINE))
        if len(coincidencias) != 1:
            errores.append(f"sin línea única {linea}")
        else:
            posiciones.append(coincidencias[0].start())
    if posiciones != sorted(posiciones):
        errores.append("líneas FPU fuera de orden")
    if "TALLER-FALLO" in texto or "Guru Meditation" in texto:
        errores.append("el firmware falló")
    return errores


def fpu_soc(qemu: str, firmware: str, chip: str) -> int:
    """M-160: espera la línea FPU completa con un plazo UART total de 30 s."""
    modo = "s3_fpu" if chip == "s3" else "fpu"
    tmp, proc, p_uart, p_puente = (lanzar_s3(qemu, firmware) if chip == "s3" else
                                 lanzar(qemu, firmware, con_puente=False))
    s_uart = s_puente = None
    texto = ""
    try:
        if p_puente is not None:
            s_puente = conectar(p_puente, proc)
        s_uart = conectar(p_uart, proc)
        fin = time.monotonic() + 30
        while (restante := fin - time.monotonic()) > 0:
            s_uart.settimeout(restante)
            try:
                trozo = s_uart.recv(4096)
            except socket.timeout:
                break
            except OSError as error:
                texto += "\nTALLER-FALLO UART: " + str(error)
                break
            if not trozo:
                break
            texto += trozo.decode(errors="replace")
            # La marca puede llegar separada de los resultados o del salto de línea.
            if re.search(r"^TALLER-OK fpu[^\n]*\n", texto, re.MULTILINE) or "TALLER-FALLO" in texto:
                break
    finally:
        proc.kill()
        proc.wait()
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        if proc.stdout is not None:
            proc.stdout.close()
        tmp.cleanup()
    errores = comprobar_fpu(texto)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {modo}\n" + texto if errores else
          f"OK {modo}: CPENABLE=0xff; FPU 105 -21 3")
    return 1 if errores else 0


def fpu(qemu: str, firmware: str) -> int:
    return fpu_soc(qemu, firmware, "esp32")


def s3_fpu(qemu: str, firmware: str) -> int:
    return fpu_soc(qemu, firmware, "s3")


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
    return CABECERA.pack(TIPO_ENTRADA, 8 if nivel == 2 else 3, 2, 0) + bytes([pin, nivel])


def comprobar_pull(tramas: list, texto: str, chip: str = "esp32") -> list[str]:
    """Comprueba lecturas UART y la secuencia de pulls efectivos por pad."""
    errores = []
    pines = [15, 16] if chip == "esp32" else [9 if chip == "c3" else 15]
    mascara = (1 << 40) - 1 if chip == "esp32" else (
        (1 << 22) - 1 if chip == "c3" else MASCARA_S3)
    observados = sum(1 << p for p in pines)
    marca = "pull" if chip == "esp32" else f"{chip}_pull"
    lineas = ["PULL-LISTO", "PULL-SEMILLA=0"]
    esperados = [(0, 0)]
    for i, pin in enumerate(pines):
        prefijo = "" if i == 0 else f"GPIO{pin}-"
        lineas += [f"{prefijo}PULL-ARRIBA=1", f"{prefijo}PULL-ABAJO=0", f"{prefijo}SIN-PULL=0"]
        esperados += [(1 << pin, 0), (0, 1 << pin), (0, 0)]
    if chip == "esp32":
        lineas += ["GPIO34-PULLUP=0"]
    lineas += ["PULL-IMPUESTO-LISTO", "IMPUESTO=0", f"TALLER-OK {marca}"]
    esperados += [(1 << pines[0], 0)]
    posiciones = []
    for linea in lineas:
        coincidencias = list(re.finditer(r"^" + re.escape(linea) + r"\r?\n", texto, re.MULTILINE))
        if len(coincidencias) != 1:
            errores.append(f"sin línea única {linea}")
        else:
            posiciones.append(coincidencias[0].start())
    if posiciones != sorted(posiciones) or "TALLER-FALLO" in texto:
        errores.append("UART fuera de orden o fallo del firmware")
    if not tramas or not es_hola(tramas[0], 8):
        errores.append("sin HOLA v8")
    if len(tramas) < 2 or tramas[1][0] != TIPO_PULL:
        errores.append("PULL debe seguir inmediatamente a HOLA")
    estados = []
    anterior = None
    for indice, (tipo, version, _, carga) in enumerate(tramas):
        if version != VERSION_PUENTE or tipo not in (1, 2, TIPO_TIEMPO, TIPO_PULL):
            errores.append("tipo o versión inválidos")
        if tipo == 1 and indice:
            errores.append("HOLA repetido")
        if tipo == TIPO_TIEMPO and carga:
            errores.append("TIEMPO con carga")
        if tipo in (2, TIPO_PULL):
            if len(carga) != 16:
                errores.append("GPIO/PULL debe tener 16 bytes")
                continue
            arriba, abajo = struct.unpack("<QQ", carga)
            if (arriba | abajo) & ~mascara:
                errores.append("máscara fuera del chip")
            if tipo == TIPO_PULL:
                if chip == "esp32" and (arriba | abajo) & (0x3f << 34):
                    errores.append("GPIO34..39 no tienen pull")
                if anterior == (arriba, abajo):
                    errores.append("PULL repetido sin cambio")
                anterior = (arriba, abajo)
                estados.append((arriba & observados, abajo & observados))
    siguiente = 0
    for estado in estados:
        if siguiente < len(esperados) and estado == esperados[siguiente]:
            siguiente += 1
    if siguiente != len(esperados):
        errores.append(f"secuencia PULL incompleta: {estados}")
    tiempos = [t for _, _, t, _ in tramas]
    if tiempos != sorted(tiempos):
        errores.append("t_ns decreciente")
    return errores


def pull_soc(qemu: str, firmware: str, chip: str) -> int:
    """Libera los pads antes de las lecturas y después impone cero con PU activo."""
    tmp, proc, p_uart, p_puente = (lanzar(qemu, firmware, True) if chip == "esp32"
                                  else lanzar_soc(qemu, firmware, chip))
    pines = [15, 16, 34] if chip == "esp32" else [9 if chip == "c3" else 15]
    marca = "pull" if chip == "esp32" else f"{chip}_pull"
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    buf = [""]

    def esperar_linea(linea, plazo_s):
        patron = r"^" + re.escape(linea) + r"\r?\n"
        fin = time.monotonic() + plazo_s
        s_uart.settimeout(0.2)
        while time.monotonic() < fin:
            if re.search(patron, buf[0], re.MULTILINE):
                return True
            try:
                trozo = s_uart.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not trozo:
                break
            buf[0] += trozo.decode(errors="replace")
        return bool(re.search(patron, buf[0], re.MULTILINE))

    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        if esperar_linea("PULL-LISTO", 60):
            # El firmware confirma el cero efectivo en todos los pads.
            s_puente.sendall(b"".join(trama_entrada(p, 0) for p in pines))
            s_uart.sendall(b"C")
            if esperar_linea("PULL-SEMILLA=0", 30):
                # El primer pad se suelta el último. Su subida con PU confirma
                # que el receptor ya procesó TODAS las liberaciones anteriores.
                s_puente.sendall(b"".join(trama_entrada(p, 2) for p in reversed(pines)))
                s_uart.sendall(b"S")
                if esperar_linea("PULL-IMPUESTO-LISTO", 30):
                    s_puente.sendall(trama_entrada(pines[0], 0))
                    s_uart.sendall(b"I")
                    esperar_linea(f"TALLER-OK {marca}", 30)
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
    errores = comprobar_pull(tramas, buf[0], chip)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {marca}\n{buf[0][-1500:]}" if errores else
          f"OK {marca}: pulls internos, entrada suelta e impuesta; HOLA/PULL v8")
    return 1 if errores else 0


def pull(qemu: str, firmware: str) -> int:
    return pull_soc(qemu, firmware, "esp32")


def c3_pull(qemu: str, firmware: str) -> int:
    return pull_soc(qemu, firmware, "c3")


def s3_pull(qemu: str, firmware: str) -> int:
    return pull_soc(qemu, firmware, "s3")


TIPO_ENTRADA_TREN = 21


def trama_entrada_tren(pin: int, pasos: list) -> bytes:
    """ENTRADA_TREN (v8): u8 pin, u16 n, n × (u32 espera_ns, u8 nivel 0/1/2)."""
    if len(pasos) > 64 or any(nivel not in (0, 1, 2) for _, nivel in pasos):
        raise ValueError("ENTRADA_TREN admite hasta 64 pasos de nivel 0, 1 o 2")
    carga = struct.pack("<BH", pin, len(pasos)) + b"".join(
        struct.pack("<IB", espera, nivel) for espera, nivel in pasos)
    return CABECERA.pack(TIPO_ENTRADA_TREN, 8, len(carga), 0) + carga


LINEAS_TREN = ["TREN-LISTO", "TREN-INICIAL=1", "FLANCOS=4", "NIVEL=0",
               "TREN-CANCELA-LISTO", "CANCELADO=1", "TALLER-OK tren"]


def comprobar_tren(texto: str) -> list[str]:
    """Cada línea una sola vez y en orden; sin TALLER-FALLO."""
    errores = []
    posiciones = []
    for linea in LINEAS_TREN:
        coincidencias = list(re.finditer(r"^" + re.escape(linea) + r"\r?\n", texto, re.MULTILINE))
        if len(coincidencias) != 1:
            errores.append(f"sin línea única {linea}")
        else:
            posiciones.append(coincidencias[0].start())
    if posiciones != sorted(posiciones) or "TALLER-FALLO" in texto:
        errores.append("UART fuera de orden o fallo del firmware")
    return errores


def tren(qemu: str, firmware: str) -> int:
    """GPIO16 con INPUT_PULLUP: un tren 0/suelto/0/suelto/0 a 300 µs da 4 flancos
    tras el primero y acaba en 0; luego un ENTRADA cancela el paso pendiente."""
    tmp, proc, p_uart, p_puente = lanzar(qemu, firmware, True)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    buf = [""]

    def esperar_linea(linea, plazo_s):
        patron = r"^" + re.escape(linea) + r"\r?\n"
        fin = time.monotonic() + plazo_s
        s_uart.settimeout(0.2)
        while time.monotonic() < fin:
            if re.search(patron, buf[0], re.MULTILINE):
                return True
            try:
                trozo = s_uart.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not trozo:
                break
            buf[0] += trozo.decode(errors="replace")
        return bool(re.search(patron, buf[0], re.MULTILINE))

    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        if esperar_linea("TREN-LISTO", 60):
            s_puente.sendall(trama_entrada_tren(16, [(0, 0), (300_000, 2), (300_000, 0),
                                                     (300_000, 2), (300_000, 0)]))
            if esperar_linea("TREN-CANCELA-LISTO", 30):
                # El paso de 20 ms a 0 nunca debe llegar: ENTRADA lo cancela.
                s_puente.sendall(trama_entrada_tren(16, [(0, 2), (20_000_000, 0)]) +
                                 trama_entrada(16, 1))
                esperar_linea("TALLER-OK tren", 30)
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
    errores = comprobar_tren(buf[0])
    if not tramas or not es_hola(tramas[0], 8):
        errores.append("sin HOLA v8")
    for error in errores:
        print("FALLO", error)
    print(f"FALLO tren\n{buf[0][-1500:]}" if errores else
          "OK tren: ENTRADA_TREN en tiempo virtual, suelto con PU y cancelación por ENTRADA")
    return 1 if errores else 0


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
                         pin_salida: int, pasos: list[tuple[int, int]], *,
                         pines: int = 40) -> bytes:
    if (not 1 <= pines <= 49 or not 0 <= pin_disparo < pines or flanco not in (0, 1) or
            not 0 <= minimo_ns <= 0xffffffff or not 0 <= pin_salida < pines or
            len(pasos) > 200 or any(not 0 <= espera <= 0xffffffff or nivel not in (0, 1)
                                    for espera, nivel in pasos)):
        raise ValueError(f"ENTRADA_ARMADA inválida: GPIO 0..{pines - 1}, flanco/nivel 0..1, u32 y n<=200")
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
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
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


def comprobar_pwm_soc(tramas: list, texto: str, chip: str, pin: int,
                       pines: int, mascara: int) -> list[str]:
    """HOLA v8, tramas PWM válidas y secuencia servo/cambio de reloj/apagado."""
    errores = []
    if not tramas or not es_hola(tramas[0], VERSION_PUENTE):
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
    if not linea_unica(texto, f"TALLER-OK {chip}_pwm") or "TALLER-FALLO" in texto:
        errores.append(f"sin línea única TALLER-OK {chip}_pwm o fallo del firmware")
    vistos = []
    for tipo, version, _, carga in tramas:
        if tipo != 4:
            continue
        if version != VERSION_PUENTE or len(carga) != 9:
            errores.append(f"trama PWM inválida: versión {version}, {len(carga)} bytes")
            continue
        gpio, frecuencia, ciclo = struct.unpack("<BII", carga)
        if (gpio >= pines or not (mascara & (1 << gpio))) or ciclo > 1_000_000 or (frecuencia == 0 and ciclo):
            errores.append(f"trama PWM imposible en {chip.upper()}: GPIO{gpio}, {frecuencia} mHz, {ciclo} ppm")
            continue
        if gpio == pin and (not vistos or vistos[-1] != (frecuencia, ciclo)):
            vistos.append((frecuencia, ciclo))
    # Cada paso, dentro de un 0,5 % (el divisor del LEDC es de punto fijo), y en orden.
    i = 0
    for frecuencia, ciclo in vistos:
        if i < len(PASOS_C3_PWM):
            f, c = PASOS_C3_PWM[i]
            if abs(frecuencia - f) <= f // 200 and abs(ciclo - c) <= max(c // 200, 0):
                i += 1
    if i != len(PASOS_C3_PWM):
        errores.append(f"GPIO{pin}: secuencia PWM incompleta o desordenada: {vistos}")
    return errores


def pwm_soc(qemu: str, firmware: str, chip: str, pin: int) -> int:
    tmp, proc, p_uart, p_puente = lanzar_soc(qemu, firmware, chip)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, [f"TALLER-OK {chip}_pwm", "TALLER-FALLO"], 60)
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
    errores = comprobar_c3_pwm(tramas, texto) if chip == "c3" else comprobar_s3_pwm(tramas, texto)
    print(f"tramas: {len(tramas)} (PWM {sum(1 for t in tramas if t[0] == 4)})")
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {chip}_pwm\n" + texto[-800:] if errores else
          f"OK {chip}_pwm: GPIO{pin} 50 Hz 5 % → 25 % → 100 Hz → apagado")
    return 1 if errores else 0


def comprobar_analogico_soc(tramas: list, texto: str, chip: str) -> list[str]:
    """HOLA v8 y lecturas completas: cero, medio y máximo, en orden."""
    errores = []
    if not tramas or not es_hola(tramas[0], VERSION_PUENTE):
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
    if not linea_unica(texto, f"TALLER-OK {chip}_analogico") or "TALLER-FALLO" in texto:
        errores.append(f"sin línea única TALLER-OK {chip}_analogico o fallo del firmware")
    lecturas = []
    for linea in texto.splitlines():
        if f"{chip.upper()}-A=" not in linea:
            continue
        m = re.fullmatch(rf"{chip.upper()}-A=([0-9]+)", linea.strip("\r"))
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


def esperar_analogico_soc(sock: socket.socket, buf: list, minimo: int, maximo: int,
                         timeout_s: float, chip: str = "c3") -> bool:
    """Espera una muestra completa posterior al envío de cada valor externo."""
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
        for muestra in re.finditer(rf"^{chip.upper()}-A=([0-9]+)\r?\n", buf[0], re.MULTILINE):
            if muestra.start() >= inicio and minimo <= int(muestra[1]) <= maximo:
                return True
    return False


def analogico_soc(qemu: str, firmware: str, chip: str, pin: int) -> int:
    """Inyecta valores crudos en ADC1 con ANALOGICO (C3 GPIO0; S3 GPIO1)."""
    tmp, proc, p_uart, p_puente = lanzar_soc(qemu, firmware, chip)
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
        pasos.append(("inicial=0", esperar_analogico_soc(s_uart, buf, 0, 0, 60, chip)))
        s_puente.sendall(trama_analogico(pin, 2048))
        pasos.append(("medio=1900..2200", esperar_analogico_soc(s_uart, buf, 1900, 2200, 10, chip)))
        s_puente.sendall(trama_analogico(pin, 4095))
        pasos.append(("alto=4000..4095", esperar_analogico_soc(s_uart, buf, 4000, 4095, 10, chip)))
        if f"TALLER-OK {chip}_analogico" not in buf[0]:
            buf[0] += leer_uart_hasta(s_uart, [f"TALLER-OK {chip}_analogico", "TALLER-FALLO"], 10)
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
    errores = comprobar_analogico_soc(tramas, buf[0], chip)
    print("pasos:", pasos)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {chip}_analogico\n" + buf[0][-800:] if errores else
          f"OK {chip}_analogico: GPIO{pin} 0 → 2048 → 4095")
    return 1 if errores else 0


def comprobar_tramas_periferico_soc(tramas: list, tipo_datos: int, chip: str) -> list[str]:
    """Valida el sobre vigente, los latidos y los GPIO auxiliares de las pruebas C3/S3."""
    errores = []
    mascara = (1 << 22) - 1 if chip == "c3" else MASCARA_S3
    if not tramas or not es_hola(tramas[0], VERSION_PUENTE):
        errores.append(f"sin HOLA v{VERSION_PUENTE}")
    for indice, (tipo, version, _, carga) in enumerate(tramas):
        if version != VERSION_PUENTE or tipo not in (1, 2, TIPO_TIEMPO, TIPO_PULL, tipo_datos):
            errores.append(f"trama {chip.upper()} inválida: tipo {tipo}, versión {version}")
        if tipo == 1 and (indice != 0 or not es_hola(tramas[indice], VERSION_PUENTE)):
            errores.append("HOLA repetido o inválido")
        if tipo == TIPO_TIEMPO and carga:
            errores.append("TIEMPO debe tener carga vacía")
        if tipo in (2, TIPO_PULL):
            if len(carga) != 16:
                errores.append("trama GPIO con longitud distinta de 16")
            elif any(valor & ~mascara for valor in struct.unpack("<QQ", carga)):
                errores.append(f"GPIO {chip.upper()} fuera de su máscara física")
    # RMT se emite al terminar, pero lleva el instante de inicio del tren. Puede
    # aparecer después de un latido más reciente; sus tiempos se validan aparte.
    tiempos = [t for tipo, _, t, _ in tramas if tipo_datos != 7 or tipo != 7]
    if tiempos != sorted(tiempos):
        errores.append("t_ns decreciente")
    return errores


def comprobar_tramas_c3(tramas: list, tipo_datos: int) -> list[str]:
    return comprobar_tramas_periferico_soc(tramas, tipo_datos, "c3")


def comprobar_spi_soc(tramas: list, texto: str, chip: str) -> list[str]:
    """Exige MOSI completo de SPI2: abcd, 40960 bytes y cuatro clocks RX."""
    errores = comprobar_tramas_periferico_soc(tramas, 3, chip)
    lineas = (f"{chip.upper()}-SPI-MISO=ffffffff", f"TALLER-OK {chip}_spi")
    posiciones = []
    for linea in lineas:
        coincidencias = list(re.finditer(r"^" + re.escape(linea) + r"\r?\n", texto, re.MULTILINE))
        if len(coincidencias) != 1:
            errores.append(f"sin línea única completa {linea}")
        else:
            posiciones.append(coincidencias[0].start())
    if posiciones != sorted(posiciones) or "TALLER-FALLO" in texto:
        errores.append("orden UART incorrecto o fallo del firmware")
    datos = bytearray()
    for tipo, _, _, carga in tramas:
        if tipo != 3:
            continue
        if not 2 <= len(carga) <= 65535 or carga[0] != 2:
            errores.append("trama SPI inválida o fuera del bus 2")
        else:
            datos.extend(carga[1:])
    esperado = b"abcd" + bytes(range(256)) * 160 + b"\0" * 4
    if datos != esperado:
        errores.append(f"MOSI SPI2 distinto: {len(datos)} bytes, esperados {len(esperado)}")
    return errores


def comprobar_c3_spi(tramas: list, texto: str) -> list[str]:
    return comprobar_spi_soc(tramas, texto, "c3")


def comprobar_s3_spi(tramas: list, texto: str) -> list[str]:
    return comprobar_spi_soc(tramas, texto, "s3")


def comprobar_i2c_soc(tramas: list, texto: str, chip: str) -> list[str]:
    """Espejo 0x3C:D0=AB, escritura 00 AF con ACK y dispositivo ausente con NACK."""
    errores = comprobar_tramas_periferico_soc(tramas, 5, chip)
    esperadas = [f"{chip.upper()}-I2C-ID=AB", f"{chip.upper()}-I2C-ACK=0",
                 f"{chip.upper()}-I2C-NACK=2", f"TALLER-OK {chip}_i2c"]
    observadas = [linea for linea in texto.splitlines()
                  if f"{chip.upper()}-I2C-" in linea or f"TALLER-OK {chip}_i2c" in linea]
    if (observadas != esperadas or any(not linea_unica(texto, linea) for linea in esperadas)
            or "TALLER-FALLO" in texto):
        errores.append(f"UART I2C incompleta, duplicada o fuera de orden: {observadas}")
    escrituras = []
    for tipo, _, _, carga in tramas:
        if tipo != 5:
            continue
        if len(carga) < 3 or carga[0] != 0 or carga[1] != 0x3c:
            errores.append("I2C_ESCRITURA inválida o fuera de bus 0, dirección 0x3C")
        escrituras.append(carga)
    if escrituras != [b"\x00\x3c\xd0", b"\x00\x3c\x00\xaf"]:
        errores.append(f"escrituras I2C distintas de D0 y 00 AF: {[c.hex() for c in escrituras]}")
    return errores


def comprobar_c3_i2c(tramas: list, texto: str) -> list[str]:
    return comprobar_i2c_soc(tramas, texto, "c3")


def comprobar_s3_i2c(tramas: list, texto: str) -> list[str]:
    return comprobar_i2c_soc(tramas, texto, "s3")


def i2c_soc(qemu: str, firmware: str, chip: str) -> int:
    """Configura el espejo antes de que el sketch C3/S3 lea el identificador."""
    tmp, proc, p_uart, p_puente = (lanzar_c3 if chip == "c3" else lanzar_s3)(qemu, firmware)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        fin = time.monotonic() + 15
        while not tramas and time.monotonic() < fin and proc.poll() is None:
            time.sleep(0.01)
        if tramas and es_hola(tramas[0], VERSION_PUENTE):
            s_puente.sendall(trama_i2c_dispositivo(0, 0x3c, True) +
                             trama_i2c_registros(0, 0x3c, 0xd0, b"\xab"))
            texto = leer_uart_hasta(s_uart, [f"TALLER-OK {chip}_i2c", "TALLER-FALLO"], 60)
            if not texto.endswith("\n"):
                texto += leer_uart_hasta(s_uart, ["\n"], 0.5)
            time.sleep(0.5)
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
    errores = comprobar_i2c_soc(tramas, texto, chip)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {chip}_i2c\n" + texto[-1000:] if errores else
          f"OK {chip}_i2c: espejo 0x3C:D0=AB; escritura 00 AF con ACK; ausente con NACK")
    return 1 if errores else 0


def c3_i2c(qemu: str, firmware: str) -> int:
    return i2c_soc(qemu, firmware, "c3")


def s3_i2c(qemu: str, firmware: str) -> int:
    return i2c_soc(qemu, firmware, "s3")


def comprobar_rmt_soc(tramas: list, texto: str, chip: str) -> list[str]:
    """Ocho y treinta LED por ping-pong: GPIO8 en C3, GPIO10 en S3."""
    errores = comprobar_tramas_periferico_soc(tramas, 7, chip)
    marcas = [linea for linea in texto.splitlines() if f"TALLER-OK {chip}_rmt" in linea]
    if (marcas != [f"TALLER-OK {chip}_rmt"] or not linea_unica(texto, f"TALLER-OK {chip}_rmt")
            or "TALLER-FALLO" in texto):
        errores.append(f"sin línea única TALLER-OK {chip}_rmt o fallo del firmware")
    trenes = []
    anterior = None
    for tipo, _, t_ns, carga in tramas:
        if tipo != 7:
            continue
        if len(carga) < 10 or len(carga) > 65535 or (len(carga) - 6) % 4:
            errores.append("trama RMT con longitud inválida")
            anterior = None
            continue
        canal, gpio, tick_ps = struct.unpack_from("<BBI", carga)
        if (canal not in range(2 if chip == "c3" else 4) or
                gpio != (8 if chip == "c3" else 10) or tick_ps != 100000):
            errores.append(f"RMT {chip.upper()}: canal={canal}, gpio={gpio}, tick_ps={tick_ps}")
        clave = (t_ns, canal, gpio, tick_ps)
        simbolos = list(struct.unpack_from(f"<{(len(carga) - 6) // 4}I", carga, 6))
        # El emisor del puente conserva el inicio de TX al trocear un tren largo.
        if clave == anterior:
            trenes[-1][1].extend(simbolos)
        else:
            trenes.append((clave, simbolos))
        anterior = clave
    if len(trenes) != 2:
        errores.append(f"se esperaban dos transmisiones RMT, recibidas {len(trenes)}")
    esperados = [[(17, 34, 51)] * 8, [(68, 85, 102)] * 30]
    fin_anterior = 0
    for indice, (clave, simbolos) in enumerate(trenes):
        if clave[0] < fin_anterior:
            errores.append("tiempos RMT decrecientes o transmisiones solapadas")
        # Cada bit exige alto/bajo de 4/8 u 8/4 ticks; EOF es un único símbolo cero.
        if (not simbolos or simbolos[-1] != 0 or
                any(s not in (0x00088004, 0x00048008) for s in simbolos[:-1])):
            errores.append(f"símbolos WS2812 o fin RMT inválidos en tren {indice}")
        fin_anterior = clave[0] + max(0, len(simbolos) - 1) * 1200
        try:
            colores = decodificar_rmt(simbolos, clave[3])
            if indice >= len(esperados) or colores != esperados[indice]:
                errores.append(f"colores o longitud RMT incorrectos en tren {indice}: {colores}")
        except ValueError as error:
            errores.append(str(error))
    return errores


def comprobar_c3_rmt(tramas: list, texto: str) -> list[str]:
    return comprobar_rmt_soc(tramas, texto, "c3")


def comprobar_s3_rmt(tramas: list, texto: str) -> list[str]:
    return comprobar_rmt_soc(tramas, texto, "s3")


def spi_soc(qemu: str, firmware: str, chip: str) -> int:
    tmp, proc, p_uart, p_puente = (lanzar_c3 if chip == "c3" else lanzar_s3)(qemu, firmware)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, [f"TALLER-OK {chip}_spi", "TALLER-FALLO"], 60)
        if not texto.endswith("\n"):
            texto += leer_uart_hasta(s_uart, ["\n"], 0.5)
        time.sleep(0.5)
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
    errores = comprobar_spi_soc(tramas, texto, chip)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {chip}_spi\n" + texto[-1000:] if errores else
          f"OK {chip}_spi: SPI2 bus 2, 40968 bytes MOSI exactos y MISO ffffffff")
    return 1 if errores else 0


def c3_spi(qemu: str, firmware: str) -> int:
    return spi_soc(qemu, firmware, "c3")


def s3_spi(qemu: str, firmware: str) -> int:
    return spi_soc(qemu, firmware, "s3")


def rmt_soc(qemu: str, firmware: str, chip: str) -> int:
    tmp, proc, p_uart, p_puente = (lanzar_c3 if chip == "c3" else lanzar_s3)(qemu, firmware)
    tramas: list = []
    parar = threading.Event()
    hilo = s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        hilo = threading.Thread(target=leer_tramas, args=(s_puente, tramas, parar), daemon=True)
        hilo.start()
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, [f"TALLER-OK {chip}_rmt", "TALLER-FALLO"], 60)
        if not texto.endswith("\n"):
            texto += leer_uart_hasta(s_uart, ["\n"], 0.5)
        time.sleep(0.5)
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
    errores = comprobar_rmt_soc(tramas, texto, chip)
    for error in errores:
        print("FALLO", error)
    print(f"FALLO {chip}_rmt\n" + texto[-1000:] if errores else
          f"OK {chip}_rmt: GPIO{8 if chip == 'c3' else 10}, 8 y 30 LED GRB completos, 100 ns por tick y fin explícito")
    return 1 if errores else 0


def c3_rmt(qemu: str, firmware: str) -> int:
    return rmt_soc(qemu, firmware, "c3")


def s3_rmt(qemu: str, firmware: str) -> int:
    return rmt_soc(qemu, firmware, "s3")


def comprobar_s3_flash_qio(texto: str) -> list[str]:
    """Borrar, escribir y leer 16 bytes con QIO en el IS25LP128 (16 MiB), sin desfase."""
    errores = []
    for linea in ("BORRADO err=0 primeros=ff ff ff ff", "ESCRITO err=0 primeros=a0 a1 a2 a3 igual=1",
                  "TAMANO=4194304 FISICO=16777216 ID=9d6018", "TALLER-OK flash"):
        if len(re.findall(r"^" + re.escape(linea) + r"\r?$", texto, re.MULTILINE)) != 1:
            errores.append(f"sin línea única {linea}")
    if "qio_mode: Failed" in texto:
        errores.append("no se activó el modo QIO")
    if "Guru Meditation" in texto or "TALLER-FALLO" in texto:
        errores.append("el firmware falló")
    return errores


def s3_flash_qio(qemu: str, firmware: str) -> int:
    """Regresión: en QIO, los ciclos dummy de 0xEB llevan 4 bits por ciclo (antes, 2 bytes de desfase)."""
    tmp, proc, p_uart, p_puente = lanzar_soc(qemu, firmware, "s3", flash_mib=16)
    s_uart = s_puente = None
    texto = ""
    try:
        s_puente = conectar(p_puente, proc)
        s_uart = conectar(p_uart, proc)
        texto = leer_uart_hasta(s_uart, ["TALLER-OK flash", "TALLER-FALLO"], 60)
        if not texto.endswith("\n"):
            texto += leer_uart_hasta(s_uart, ["\n"], 0.5)
    finally:
        proc.kill()
        proc.wait()
        for conexion in (s_puente, s_uart):
            if conexion is not None:
                conexion.close()
        if proc.stdout is not None:
            proc.stdout.close()
        tmp.cleanup()
    errores = comprobar_s3_flash_qio(texto)
    for error in errores:
        print("FALLO", error)
    print("FALLO s3_flash_qio\n" + texto[-1500:] if errores else
          "OK s3_flash_qio: IS25LP128 en QIO, borrar/escribir/leer sin desfase")
    return 1 if errores else 0

# Los GPIO físicos S3 dejan un hueco en los índices 22..25.
MASCARA_S3 = 0x1fffffc3fffff


def lanzar_c3(qemu: str, firmware: str):
    return lanzar_soc(qemu, firmware, "c3")


def lanzar_s3(qemu: str, firmware: str):
    return lanzar_soc(qemu, firmware, "s3")


def comprobar_c3_gpio(tramas: list, texto: str) -> list[str]:
    return comprobar_gpio_soc(tramas, texto, "c3", 8, 22, (1 << 22) - 1)


def comprobar_s3_gpio(tramas: list, texto: str) -> list[str]:
    return comprobar_gpio_soc(tramas, texto, "s3", 2, 49, MASCARA_S3)


def comprobar_c3_pwm(tramas: list, texto: str) -> list[str]:
    return comprobar_pwm_soc(tramas, texto, "c3", 8, 22, (1 << 22) - 1)


def comprobar_s3_pwm(tramas: list, texto: str) -> list[str]:
    return comprobar_pwm_soc(tramas, texto, "s3", 4, 49, MASCARA_S3)


def comprobar_c3_analogico(tramas: list, texto: str) -> list[str]:
    return comprobar_analogico_soc(tramas, texto, "c3")


def comprobar_s3_analogico(tramas: list, texto: str) -> list[str]:
    return comprobar_analogico_soc(tramas, texto, "s3")


def c3_gpio(qemu: str, firmware: str) -> int:
    return gpio_soc(qemu, firmware, "c3", 8, 9)


def s3_gpio(qemu: str, firmware: str) -> int:
    return gpio_soc(qemu, firmware, "s3", 2, 4)


def c3_pwm(qemu: str, firmware: str) -> int:
    return pwm_soc(qemu, firmware, "c3", 8)


def s3_pwm(qemu: str, firmware: str) -> int:
    return pwm_soc(qemu, firmware, "s3", 4)


def c3_analogico(qemu: str, firmware: str) -> int:
    return analogico_soc(qemu, firmware, "c3", 0)


def s3_analogico(qemu: str, firmware: str) -> int:
    return analogico_soc(qemu, firmware, "s3", 1)


if __name__ == "__main__":
    qemu, modo = sys.argv[1], sys.argv[2]
    if modo in ("pull", "c3_pull", "s3_pull", "tren"):
        sys.exit(globals()[modo](qemu, sys.argv[3]))
    if modo in ("s3_gpio", "s3_pwm", "s3_analogico", "s3_i2c", "s3_rmt", "s3_spi", "s3_fpu", "s3_flash_qio"):
        sys.exit(globals()[modo](qemu, sys.argv[3]))
    if modo == "c3_gpio":
        sys.exit(c3_gpio(qemu, sys.argv[3]))
    if modo == "c3_pwm":
        sys.exit(c3_pwm(qemu, sys.argv[3]))
    if modo == "c3_analogico":
        sys.exit(c3_analogico(qemu, sys.argv[3]))
    if modo == "c3_i2c":
        sys.exit(c3_i2c(qemu, sys.argv[3]))
    if modo == "c3_rmt":
        sys.exit(c3_rmt(qemu, sys.argv[3]))
    if modo == "c3_spi":
        sys.exit(c3_spi(qemu, sys.argv[3]))
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
    if modo == "fpu":
        sys.exit(fpu(qemu, sys.argv[3]))
    if modo == "spi_dma":
        sys.exit(spi_dma(qemu, sys.argv[3]))
    sys.exit(puente(qemu, sys.argv[3]))
