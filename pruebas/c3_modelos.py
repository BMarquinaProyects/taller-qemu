"""Compila los modelos C3 reales con adaptadores de servicios QEMU mínimos."""
import shutil
import subprocess
import tempfile
from pathlib import Path


def ejecutar_modelo(caso, nombre, carpeta="misc"):
    cc = (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or
          shutil.which("cl"))
    if cc is None:
        caso.skipTest("compilador C no disponible; ejecutar en WSL")
    raiz = Path(__file__).resolve().parents[1]
    fuente = raiz / f"capas/hw/{carpeta}/esp32c3_{nombre}.c"
    caso.assertTrue(fuente.exists(), f"falta modelo C3 {nombre}")
    cab = (raiz / f"capas/include/hw/{carpeta}/esp32c3_{nombre}.h").read_text(encoding="utf-8")
    cab = "\n".join(l for l in cab.splitlines() if not l.startswith(("#include", "#pragma")))
    codigo = fuente.read_text(encoding="utf-8")
    inicio = codigo.index("/* Estado y registros")
    fin = codigo.index("static const MemoryRegionOps")
    reset = codigo[codigo.index(f"static void esp32c3_{nombre}_reiniciar"):
                   codigo.index(f"static void esp32c3_{nombre}_init")]
    plantilla = (raiz / f"pruebas/c3_{nombre}_modelo.c").read_text(encoding="utf-8")
    codigo = plantilla.replace("/* CABECERA_REAL */", cab).replace(
        "/* MODELO_REAL */", codigo[inicio:fin] + reset)
    compilar_ejecutar(codigo, cc)


def compilar_ejecutar(codigo, cc):
    """Compila y ejecuta una unidad C aislada, también desde MSVC nativo."""
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "modelo.c"
        msvc = Path(cc).stem.lower() == "cl"
        exe = Path(tmp) / ("modelo.exe" if msvc else "modelo")
        c.write_text(codigo, encoding="utf-8")
        if msvc:
            # Ejecutar desde una consola de desarrollo de Visual Studio.
            # Como GCC sin -Wconversion, permitir estrechamientos de registros.
            comando = [cc, "/nologo", "/std:c11", "/W4", "/WX", "/wd4100",
                       "/wd4244", "/wd4267",
                       str(c), f"/Fe:{exe}", f"/Fo:{Path(tmp) / 'modelo.obj'}"]
        else:
            comando = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", str(c), "-o", str(exe)]
        subprocess.run(comando, check=True)
        subprocess.run([str(exe)], check=True)
