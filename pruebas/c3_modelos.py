"""Compila los modelos C3 reales con adaptadores de servicios QEMU mínimos."""
import shutil
import subprocess
import tempfile
from pathlib import Path


def ejecutar_modelo(caso, nombre):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        caso.skipTest("compilador C no disponible; ejecutar en WSL")
    raiz = Path(__file__).resolve().parents[1]
    fuente = raiz / f"capas/hw/misc/esp32c3_{nombre}.c"
    caso.assertTrue(fuente.exists(), f"falta modelo C3 {nombre}")
    cab = (raiz / f"capas/include/hw/misc/esp32c3_{nombre}.h").read_text(encoding="utf-8")
    cab = "\n".join(l for l in cab.splitlines() if not l.startswith(("#include", "#pragma")))
    codigo = fuente.read_text(encoding="utf-8")
    inicio = codigo.index("/* Estado y registros")
    fin = codigo.index("static const MemoryRegionOps")
    reset = codigo[codigo.index(f"static void esp32c3_{nombre}_reiniciar"):
                   codigo.index(f"static void esp32c3_{nombre}_init")]
    plantilla = (raiz / f"pruebas/c3_{nombre}_modelo.c").read_text(encoding="utf-8")
    codigo = plantilla.replace("/* CABECERA_REAL */", cab).replace(
        "/* MODELO_REAL */", codigo[inicio:fin] + reset)
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "modelo.c", Path(tmp) / "modelo"
        c.write_text(codigo, encoding="utf-8")
        subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
