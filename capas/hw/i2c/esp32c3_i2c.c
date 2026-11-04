/* Maestro I2C0 C3/S3: bus QEMU normal, FIFO APB y comandos del HAL IDF 5.5.
 * Registros contrastados con soc/i2c_reg.h y hal/i2c_ll.h Arduino 3.3.12.
 * GPL-2.0-or-later. Sin modo esclavo, DMA, temporización ni señales GPIO.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/i2c/esp32c3_i2c.h"

/* Estado y registros independientes del I2C clásico. */
#define C3_I2C_CTR 0x04
#define C3_I2C_SR 0x08
#define C3_I2C_FIFO_ST 0x14
#define C3_I2C_FIFO_CONF 0x18
#define C3_I2C_DATA 0x1c
#define C3_I2C_RAW 0x20
#define C3_I2C_CLR 0x24
#define C3_I2C_ENA 0x28
#define C3_I2C_ST 0x2c
#define C3_I2C_CMD 0x58
#define C3_I2C_MASTER (1u << 4)
#define C3_I2C_START (1u << 5)
#define C3_I2C_FSM_RST (1u << 10)
#define C3_I2C_UPDATE (1u << 11)
#define C3_I2C_DONE (1u << 31)
#define C3_I2C_END (1u << 3)
#define C3_I2C_COMPLETE (1u << 7)
#define C3_I2C_NACK (1u << 10)

static bool esp32c3_i2c_registro_valido(hwaddr addr)
{
    return !(addr & 3) && (addr <= 0x84 || addr == 0xf8) && addr != 0x3c;
}

static void esp32c3_i2c_actualizar_irq(ESP32C3I2CState *s)
{
    uint32_t conf = s->registros[C3_I2C_FIFO_CONF / 4];
    uint32_t raw = s->registros[C3_I2C_RAW / 4] & ~3u;

    /* Las marcas FIFO son condiciones de nivel; CLR no las desactiva. */
    if (s->rx_cantidad && s->rx_cantidad >= (conf & 31)) {
        raw |= 1;
    }
    if (s->tx_cantidad <= ((conf >> 5) & 31)) {
        raw |= 2;
    }
    s->registros[C3_I2C_RAW / 4] = raw;
    s->registros[C3_I2C_ST / 4] = raw & s->registros[C3_I2C_ENA / 4];
    qemu_set_irq(s->irq, s->registros[C3_I2C_ST / 4] != 0);
}

static void esp32c3_i2c_finalizar(ESP32C3I2CState *s)
{
    if (s->bus) {
        i2c_end_transfer(s->bus);
    }
    s->ocupado = false;
    s->espera_direccion = true;
}

static bool esp32c3_i2c_comprobar_ack(ESP32C3I2CState *s, uint32_t cmd,
                                     bool nack)
{
    s->respuesta_ack = !nack;
    if ((cmd & (1u << 8)) && nack != !!(cmd & (1u << 9))) {
        s->registros[C3_I2C_RAW / 4] |= C3_I2C_NACK;
        s->ejecutando = false;
        esp32c3_i2c_finalizar(s);
        return false;
    }
    return true;
}

static void esp32c3_i2c_ejecutar(ESP32C3I2CState *s)
{
    while (s->ejecutando && s->comando < ESP32C3_I2C_COMANDOS) {
        uint32_t *registro = &s->registros[C3_I2C_CMD / 4 + s->comando];
        uint32_t cmd = *registro;
        unsigned op = (cmd >> 11) & 7;
        unsigned cantidad = cmd & 255;

        switch (op) {
        case 6: /* RSTART: el core QEMU debe volver a resolver la dirección. */
            esp32c3_i2c_finalizar(s);
            break;
        case 1: /* WRITE: el primer byte tras START/RSTART es la dirección. */
            while (s->bytes_hechos < cantidad) {
                uint8_t dato;
                bool nack;

                if (!s->tx_cantidad) {
                    goto esperar_fifo;
                }
                dato = s->tx[s->tx_lectura];
                s->tx_lectura = (s->tx_lectura + 1) % ESP32C3_I2C_FIFO;
                s->tx_cantidad--;
                s->bytes_hechos++;
                if (s->espera_direccion) {
                    nack = i2c_start_transfer(s->bus, dato >> 1, dato & 1) != 0;
                    s->espera_direccion = false;
                    s->ocupado = true;
                } else {
                    nack = i2c_send(s->bus, dato) != 0;
                }
                if (!esp32c3_i2c_comprobar_ack(s, cmd, nack)) {
                    goto esperar_fifo;
                }
            }
            break;
        case 3: /* READ: pausa al llenar FIFO; leer DATA permite continuar. */
            while (s->bytes_hechos < cantidad) {
                if (s->rx_cantidad == ESP32C3_I2C_FIFO) {
                    goto esperar_fifo;
                }
                s->rx[s->rx_escritura] = i2c_recv(s->bus);
                s->rx_escritura = (s->rx_escritura + 1) % ESP32C3_I2C_FIFO;
                s->rx_cantidad++;
                s->bytes_hechos++;
                if (cmd & (1u << 10)) {
                    i2c_nack(s->bus);
                }
            }
            break;
        case 2: /* STOP completa la transacción del usuario. */
            esp32c3_i2c_finalizar(s);
            s->registros[C3_I2C_RAW / 4] |= C3_I2C_COMPLETE;
            s->ejecutando = false;
            break;
        case 4: /* END permite otro lote sin STOP ni otra dirección. */
            s->registros[C3_I2C_RAW / 4] |= C3_I2C_END;
            s->ejecutando = false;
            break;
        default:
            /* Un comando desconocido nunca se convierte en un éxito. */
            s->registros[C3_I2C_RAW / 4] |= 1u << 8;
            s->ejecutando = false;
            esp32c3_i2c_finalizar(s);
            goto esperar_fifo;
        }
        *registro |= C3_I2C_DONE;
        s->comando++;
        s->bytes_hechos = 0;
    }
    if (s->comando == ESP32C3_I2C_COMANDOS && s->ejecutando) {
        s->ejecutando = false;
        s->registros[C3_I2C_RAW / 4] |= C3_I2C_END;
    }
esperar_fifo:
    if (!s->ejecutando) {
        s->registros[C3_I2C_CTR / 4] &= ~C3_I2C_START;
    }
    esp32c3_i2c_actualizar_irq(s);
}

static uint64_t esp32c3_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3I2CState *s = ESP32C3_I2C(opaque);

    if (!esp32c3_i2c_registro_valido(addr)) {
        return 0;
    }
    switch (addr) {
    case C3_I2C_SR:
        /*
         * RESP_REC: los .h de ESP-IDF dicen «0: ACK, 1: NACK», pero el C3 y el S3 reales dan
         * 1 cuando el esclavo contesta ACK (medido en un C3 rev v0.4, esp32sim PR #282). esp-hal
         * 1.x se apoya en eso: si TRANS_COMPLETE llega con RESP_REC a 0, devuelve
         * AcknowledgeCheckFailed(Data). ESP-IDF, Arduino y MicroPython no lo leen.
         */
        return s->respuesta_ack | ((uint32_t)s->ocupado << 4) |
               (s->rx_cantidad << 8) | (s->tx_cantidad << 18);
    case C3_I2C_FIFO_ST:
        return s->rx_lectura | (s->rx_escritura << 5) |
               (s->tx_lectura << 10) | (s->tx_escritura << 15);
    case C3_I2C_DATA: {
        uint8_t dato = 0;

        if (s->rx_cantidad) {
            dato = s->rx[s->rx_lectura];
            s->rx_lectura = (s->rx_lectura + 1) % ESP32C3_I2C_FIFO;
            s->rx_cantidad--;
        } else {
            s->registros[C3_I2C_RAW / 4] |= 1u << 12;
        }
        esp32c3_i2c_ejecutar(s);
        return dato;
    }
    case C3_I2C_CLR:
        return 0;
    default:
        return s->registros[addr / 4];
    }
}

static void esp32c3_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    ESP32C3I2CState *s = ESP32C3_I2C(opaque);
    uint32_t v = (uint32_t)value;

    if (!esp32c3_i2c_registro_valido(addr)) {
        return;
    }
    switch (addr) {
    case C3_I2C_SR:
    case C3_I2C_FIFO_ST:
    case C3_I2C_RAW:
    case C3_I2C_ST:
    case 0xf8: /* DATE. */
        return;
    case C3_I2C_CTR:
        s->registros[addr / 4] = v & ~(C3_I2C_UPDATE | C3_I2C_FSM_RST);
        if (v & C3_I2C_FSM_RST) {
            s->ejecutando = false;
            s->comando = s->bytes_hechos = 0;
            s->respuesta_ack = false;
            s->registros[addr / 4] &= ~C3_I2C_START;
            esp32c3_i2c_finalizar(s);
        } else if ((v & C3_I2C_START) && (v & C3_I2C_MASTER)) {
            s->ejecutando = true;
            s->comando = s->bytes_hechos = 0;
            for (unsigned n = 0; n < ESP32C3_I2C_COMANDOS; n++) {
                s->registros[C3_I2C_CMD / 4 + n] &= ~C3_I2C_DONE;
            }
            esp32c3_i2c_ejecutar(s);
        } else if (!(v & C3_I2C_MASTER)) {
            s->registros[addr / 4] &= ~C3_I2C_START;
        }
        break;
    case C3_I2C_FIFO_CONF:
        s->registros[addr / 4] = v & ~((1u << 12) | (1u << 13));
        if (v & (1u << 12)) {
            s->rx_lectura = s->rx_escritura = s->rx_cantidad = 0;
        }
        if (v & (1u << 13)) {
            s->tx_lectura = s->tx_escritura = s->tx_cantidad = 0;
        }
        break;
    case C3_I2C_DATA:
        if (s->tx_cantidad == ESP32C3_I2C_FIFO) {
            s->registros[C3_I2C_RAW / 4] |= 1u << 11;
        } else {
            s->tx[s->tx_escritura] = (uint8_t)v;
            s->tx_escritura = (s->tx_escritura + 1) % ESP32C3_I2C_FIFO;
            s->tx_cantidad++;
            esp32c3_i2c_ejecutar(s);
        }
        break;
    case C3_I2C_CLR:
        s->registros[C3_I2C_RAW / 4] &= ~v;
        break;
    case C3_I2C_ENA:
        s->registros[addr / 4] = v & 0x3ffff;
        break;
    case C3_I2C_CMD:
    case C3_I2C_CMD + 4:
    case C3_I2C_CMD + 8:
    case C3_I2C_CMD + 12:
    case C3_I2C_CMD + 16:
    case C3_I2C_CMD + 20:
    case C3_I2C_CMD + 24:
    case C3_I2C_CMD + 28:
        s->registros[addr / 4] = v & 0x3fff;
        break;
    case 0x80: /* Recuperación del bus: el pulso SCL_RST_SLV_EN termina. */
        s->registros[addr / 4] = v & ~1u;
        break;
    default:
        s->registros[addr / 4] = v;
        break;
    }
    esp32c3_i2c_actualizar_irq(s);
}

static const MemoryRegionOps esp32c3_i2c_ops = {
    .read = esp32c3_i2c_read,
    .write = esp32c3_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_i2c_reiniciar(Object *obj, ResetType tipo)
{
    ESP32C3I2CState *s = ESP32C3_I2C(obj);

    esp32c3_i2c_finalizar(s);
    memset(s->registros, 0, sizeof(s->registros));
    s->tx_lectura = s->tx_escritura = s->tx_cantidad = 0;
    s->rx_lectura = s->rx_escritura = s->rx_cantidad = 0;
    s->comando = s->bytes_hechos = 0;
    s->ejecutando = s->respuesta_ack = false;
    esp32c3_i2c_actualizar_irq(s);
    /* El esclavo conserva su espejo y puntero durante el reset del maestro. */
}

static void esp32c3_i2c_init(Object *obj)
{
    ESP32C3I2CState *s = ESP32C3_I2C(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_i2c_ops, s,
                          TYPE_ESP32C3_I2C, ESP32C3_I2C_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void esp32c3_i2c_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_i2c_reiniciar;
}

static const TypeInfo esp32c3_i2c_info = {
    .name = TYPE_ESP32C3_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3I2CState),
    .instance_init = esp32c3_i2c_init,
    .class_init = esp32c3_i2c_class_init,
};

static void esp32c3_i2c_register_types(void)
{
    type_register_static(&esp32c3_i2c_info);
}

type_init(esp32c3_i2c_register_types)
