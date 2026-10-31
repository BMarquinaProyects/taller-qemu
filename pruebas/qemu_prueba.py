"""
Pruebas de humo de taller-qemu: ejecuta un firmware y comprueba su salida (y las tramas del puente).

  python pruebas/qemu_prueba.py <qemu-system-xtensa> humo   <firmware.bin> <marca>
  python pruebas/qemu_prueba.py <qemu-system-xtensa> puente <puente_prueba.flash.bin>

Sale con 0 si todo cuadra. Sin dependencias fuera de la biblioteca estándar.
"""
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
    if not tramas or tramas[0][0] != 1 or struct.unpack("<I", tramas[0][3])[0] != 1:
        errores.append(f"la primera trama no es HOLA v1: {tramas[:1]}")
    if any(v != 1 for _, v, _, _ in tramas):
        errores.append("tramas con versión distinta de 1")
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


if __name__ == "__main__":
    qemu, modo = sys.argv[1], sys.argv[2]
    if modo == "humo":
        sys.exit(humo(qemu, sys.argv[3], sys.argv[4]))
    sys.exit(puente(qemu, sys.argv[3]))
