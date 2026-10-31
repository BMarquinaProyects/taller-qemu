/*
 * ADC1 del ESP32 por el controlador RTC (SENS).
 * Conversión única inmediata; el resto de registros conserva lo escrito.
 *
 * Copyright (c) 2026 Taller. Licencia: GPL-2.0-or-later.
 */
#include "qemu/osdep.h"
#include "hw/registerfields.h"
#include "hw/misc/esp32_sens.h"

REG32(SENS_SAR_MEAS_START1, 0x54)
    FIELD(SENS_SAR_MEAS_START1, DATA_SAR, 0, 16)
    FIELD(SENS_SAR_MEAS_START1, DONE_SAR, 16, 1)
    FIELD(SENS_SAR_MEAS_START1, START_SAR, 17, 1)
    FIELD(SENS_SAR_MEAS_START1, START_FORCE, 18, 1)
    FIELD(SENS_SAR_MEAS_START1, EN_PAD, 19, 12)
    FIELD(SENS_SAR_MEAS_START1, EN_PAD_FORCE, 31, 1)

/* Un ESP32 por máquina, igual que las entradas externas del GPIO. */
static Esp32SensState *sens_del_chip;

void esp32_sens_fijar_valor(int canal, uint16_t valor)
{
    if (!sens_del_chip || canal < 0 || canal >= ESP32_SENS_CANALES_ADC1) {
        return;
    }
    sens_del_chip->valores[canal] = MIN(valor, 4095);
}

static uint64_t esp32_sens_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);

    return s->registros[addr / 4];
}

static void esp32_sens_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);
    uint32_t valor = value;

    if (addr == A_SENS_SAR_MEAS_START1) {
        uint32_t anterior = s->registros[R_SENS_SAR_MEAS_START1];
        uint32_t solo_lectura = R_SENS_SAR_MEAS_START1_DATA_SAR_MASK |
                               R_SENS_SAR_MEAS_START1_DONE_SAR_MASK;

        valor = (valor & ~solo_lectura) | (anterior & solo_lectura);
        if (!FIELD_EX32(valor, SENS_SAR_MEAS_START1, START_SAR)) {
            valor &= ~R_SENS_SAR_MEAS_START1_DONE_SAR_MASK;
        } else if (!FIELD_EX32(anterior, SENS_SAR_MEAS_START1, START_SAR)) {
            uint32_t pads = FIELD_EX32(valor, SENS_SAR_MEAS_START1, EN_PAD);
            uint16_t dato = 0;

            /* El bit más bajo manda; sin canal ADC1 válido se devuelve cero. */
            for (int canal = 0; canal < ESP32_SENS_CANALES_ADC1; canal++) {
                if (pads & (1u << canal)) {
                    dato = s->valores[canal];
                    break;
                }
            }
            valor = (valor & ~solo_lectura) |
                    R_SENS_SAR_MEAS_START1_DONE_SAR_MASK | dato;
        }
    }
    s->registros[addr / 4] = valor;
}

static const MemoryRegionOps esp32_sens_ops = {
    .read = esp32_sens_read,
    .write = esp32_sens_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32_sens_reiniciar(Object *obj, ResetType tipo)
{
    Esp32SensState *s = ESP32_SENS(obj);

    memset(s->registros, 0, sizeof(s->registros));
    /* Las señales externas siguen conectadas durante el reset del chip. */
}

static void esp32_sens_init(Object *obj)
{
    Esp32SensState *s = ESP32_SENS(obj);

    sens_del_chip = s;
    memory_region_init_io(&s->iomem, obj, &esp32_sens_ops, s,
                          TYPE_ESP32_SENS, ESP32_SENS_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void esp32_sens_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_sens_reiniciar;
}

static const TypeInfo esp32_sens_info = {
    .name = TYPE_ESP32_SENS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SensState),
    .instance_init = esp32_sens_init,
    .class_init = esp32_sens_class_init,
};

static void esp32_sens_register_types(void)
{
    type_register_static(&esp32_sens_info);
}

type_init(esp32_sens_register_types)
