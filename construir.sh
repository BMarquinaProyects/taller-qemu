#!/usr/bin/env bash
# Compila el QEMU de Espressif con las capas de Taller y lo empaqueta.
#
#   ./construir.sh                 descarga el código base, aplica capas/, compila y empaqueta en dist/
#   TARBALL=/ruta/src.tar.xz ./construir.sh    usa un tarball ya descargado
#   TRABAJO=~/qemu-trabajo ./construir.sh      carpeta de trabajo (por defecto .trabajo/; en WSL conviene ~/…)
#
# Funciona en Linux (Ubuntu 24.04), WSL y MSYS2 UCRT64 (Windows; MINGW64 ya no trae libslirp).
# La compilación es incremental: si la carpeta de trabajo ya existe, solo recompila lo que cambió.
set -euo pipefail

AQUI="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(tr -d '[:space:]' < "$AQUI/VERSION")"
BASE="esp-develop-9.2.2-20260417"
NOMBRE_SRC="qemu-esp_develop_9.2.2_20260417-src"
SHA256="66015182274b9fda7d3c7f19e8f160e6e23947217cc149c7aa97e1586dfc6198"
URL="https://github.com/espressif/qemu/releases/download/${BASE}/${NOMBRE_SRC}.tar.xz"

TRABAJO="${TRABAJO:-$AQUI/.trabajo}"
SALIDA="${SALIDA:-$AQUI/dist}"
TARBALL="${TARBALL:-$TRABAJO/${NOMBRE_SRC}.tar.xz}"

if [[ "${MSYSTEM:-}" == "UCRT64" || "${MSYSTEM:-}" == "MINGW64" ]]; then HOST="x86_64-w64-mingw32"; EXE=".exe"; else HOST="x86_64-linux-gnu"; EXE=""; fi
paso() { printf '\n==> %s\n' "$*"; }

mkdir -p "$TRABAJO" "$SALIDA"

# 1. Código base (verificado)
if [[ ! -f "$TARBALL" ]]; then
  paso "Descargando $URL"
  curl -fL --retry 3 -o "$TARBALL.parcial" "$URL" && mv "$TARBALL.parcial" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c -

SRC="$TRABAJO/$NOMBRE_SRC"
if [[ ! -f "$SRC/configure" ]]; then
  paso "Extrayendo el código base"
  if [[ -n "$EXE" ]]; then
    # MSYS2 no puede crear enlaces simbólicos: los pocos que quedan (subproyectos solo de Linux)
    # se crean como accesos directos; roms/ y tests/lcitool/ no hacen falta para compilar.
    MSYS=winsymlinks:lnk tar -xJf "$TARBALL" -C "$TRABAJO"       --exclude="$NOMBRE_SRC/roms" --exclude="$NOMBRE_SRC/tests/lcitool"
  else
    tar -xJf "$TARBALL" -C "$TRABAJO"
  fi
fi

# 2. Capas de Taller encima (solo copia lo que cambió, para no forzar recompilaciones)
paso "Aplicando capas/"
( cd "$AQUI/capas" && find . -type f ) | while read -r f; do
  if ! cmp -s "$AQUI/capas/$f" "$SRC/$f"; then
    mkdir -p "$(dirname "$SRC/$f")"
    cp "$AQUI/capas/$f" "$SRC/$f"
    echo "   $f"
  fi
done

# 3. Configurar (una vez) y compilar
BUILD="$TRABAJO/build-$HOST"
INST="$TRABAJO/instalar-$HOST"
# Prefijo no raíz: con --prefix=/ QEMU no encuentra share/qemu (ROM del ESP32) al reubicarse.
OPCIONES=(--target-list=xtensa-softmmu --prefix=/qemu --bindir=bin --datadir=share
          --enable-gcrypt --enable-slirp
          --disable-sdl --disable-gtk --disable-vnc --disable-docs
          --disable-werror --disable-capstone --disable-user
          --disable-tools --disable-guest-agent)
if [[ -n "$EXE" ]]; then
  # Windows: enlazado estático, como la receta de Espressif (.github/workflows/scripts/configure-win.sh).
  # Con enlazado dinámico, la glib de MSYS2 da "multiple definition of g_main_context_ref" (E-022).
  # Prefijo = la carpeta de instalación con ruta de Windows: con DESTDIR, MSYS2 convierte /qemu
  # en una ruta de Windows y la instalación acaba en otro sitio (E-023). QEMU es reubicable.
  OPCIONES=(--target-list=xtensa-softmmu --prefix="$(cygpath -m "$INST")/qemu" --bindir=bin --datadir=share
            --static --without-default-features
            --enable-gcrypt --enable-pixman --enable-slirp --enable-stack-protector)
fi
if [[ ! -f "$BUILD/build.ninja" || "$(cat "$BUILD/.opciones-taller" 2>/dev/null)" != "${OPCIONES[*]}" ]]; then
  paso "Configurando"
  mkdir -p "$BUILD"
  ( cd "$BUILD" && "$SRC/configure" "${OPCIONES[@]}" ) || { cat "$BUILD/meson-logs/meson-log.txt" | tail -60; false; }
  if [[ -n "$EXE" ]]; then
    # pkg-config de libgcrypt da rutas Unix y bibliotecas dinámicas (.dll.a) para libintl/libiconv:
    # se cambian por las estáticas con ruta de Windows (mismo arreglo que Espressif).
    BASE_MSYS="$(cygpath -m /)"
    PREF="${MSYSTEM_PREFIX:-/ucrt64}"
    sed -i "s|${PREF}/lib/libintl.dll.a|${BASE_MSYS}${PREF#/}/lib/libintl.a|g; s|${PREF}/lib/libiconv.dll.a|${BASE_MSYS}${PREF#/}/lib/libiconv.a|g" "$BUILD/build.ninja"
  fi
  echo "${OPCIONES[*]}" > "$BUILD/.opciones-taller"
fi
paso "Compilando"
ninja -C "$BUILD" "qemu-system-xtensa${EXE}"

# 4. Instalar en una carpeta temporal y empaquetar con la estructura del de Espressif: qemu/bin, qemu/share
paso "Empaquetando"
rm -rf "${INST:?}"
if [[ -n "$EXE" ]]; then
  ninja -C "$BUILD" install > /dev/null                  # prefijo = $INST/qemu
else
  DESTDIR="$INST" ninja -C "$BUILD" install > /dev/null  # → $INST/qemu/bin, $INST/qemu/share
fi
if [[ -n "$EXE" ]]; then
  # DLL de MSYS2 que necesita el ejecutable
  ldd "$INST/qemu/bin/qemu-system-xtensa.exe" | awk -v p="${MSYSTEM_PREFIX:-/ucrt64}/" 'index($3, p) == 1 {print $3}' | sort -u | while read -r dll; do
    cp "$dll" "$INST/qemu/bin/"
  done
fi
cp "$AQUI/LICENSE" "$AQUI/AVISO.md" "$INST/qemu/"
ARCHIVO="$SALIDA/qemu-taller-${VERSION}-${HOST}.tar.xz"
tar -cJf "$ARCHIVO" -C "$INST" qemu
echo "OK -> $ARCHIVO"
echo "   binario: $INST/qemu/bin/qemu-system-xtensa${EXE}"
