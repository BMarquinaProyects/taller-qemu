"""Ejecuta los modelos S3 reales aislando únicamente servicios de QEMU."""
import shutil
import subprocess
import tempfile
from pathlib import Path


def ejecutar_modelo(caso, nombre):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        caso.skipTest("compilador C no disponible; ejecutar en WSL")
    raiz = Path(__file__).resolve().parents[1]
    chip = "esp32c3" if nombre == "ledc" else "esp32s3"
    carpeta = "gpio" if nombre == "gpio" else "misc"
    fuente = raiz / f"capas/hw/{carpeta}/{chip}_{nombre}.c"
    caso.assertTrue(fuente.exists(), f"falta modelo S3 {nombre}")
    cab = (raiz / f"capas/include/hw/{carpeta}/{chip}_{nombre}.h").read_text(encoding="utf-8")
    cab = "\n".join(l for l in cab.splitlines() if not l.startswith(("#include", "#pragma")))
    codigo = fuente.read_text(encoding="utf-8")
    inicio = codigo.index("static bool esp32s3_gpio_pin_valido" if nombre == "gpio" else
                          "/* Estado y registros")
    fin = codigo.index("static const MemoryRegionOps")
    reiniciar = f"static void {chip}_{nombre}_" + ("reset_hold" if nombre == "gpio" else "reiniciar")
    reset = codigo[codigo.index(reiniciar):codigo.index(f"static void {chip}_{nombre}_init")]
    plantilla = "c3_ledc_modelo.c" if nombre == "ledc" else f"s3_{nombre}_modelo.c"
    modelo = (raiz / "pruebas" / plantilla).read_text(encoding="utf-8")
    modelo = modelo.replace("/* CABECERA_REAL */", cab).replace("/* MODELO_REAL */", codigo[inicio:fin] + reset)
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "modelo.c", Path(tmp) / "modelo"
        c.write_text(modelo, encoding="utf-8")
        subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-DMODELO_S3", "-I", str(raiz / "capas/include"),
                        str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
