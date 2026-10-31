# taller-qemu

QEMU de Espressif para ESP32 con periféricos extra (GPIO real, puente de GPIO/SPI) que usa
**Taller**, un simulador de placas y componentes. Ver `AVISO.md` para el origen y los cambios.

## Compilar

Linux / WSL (Ubuntu 24.04):

```
sudo apt install build-essential ninja-build pkg-config python3-venv flex bison \
  libglib2.0-dev libpixman-1-dev libslirp-dev libgcrypt20-dev
TRABAJO=~/qemu-trabajo ./construir.sh
```

Windows: MSYS2 **UCRT64** (MINGW64 ya no trae `libslirp`) con los paquetes `mingw-w64-ucrt-x86_64-{gcc,glib2,pixman,libslirp,libgcrypt,ninja,meson,python,pkgconf}`.
Ver `.github/workflows/qemu.yml`.

## Protocolo del puente (v1)

Con `-chardev socket,id=taller-puente,host=127.0.0.1,port=<p>,server=on,wait=on`, QEMU envía tramas
little-endian con una cabecera de 12 bytes: `u8 tipo, u8 version=1, u16 longitud_carga, u64 t_ns`.

| tipo | carga |
|---|---|
| 1 HOLA | `u32 version_protocolo` |
| 2 GPIO | `u64 niveles, u64 salidas` (bit n = GPIO n) |
| 3 SPI  | `u8 bus` (2 = HSPI, 3 = VSPI) + bytes MOSI de una transacción |

Sin ese chardev, QEMU se comporta como el de Espressif.
