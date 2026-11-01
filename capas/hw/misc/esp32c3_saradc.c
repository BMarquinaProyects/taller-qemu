/* ADC1 C3, una lectura por software. GPL-2.0-or-later.
 * Registros contrastados con apb_saradc_reg.h y adc_ll.h del core 3.3.12.
 * Sin ADC2, DMA, latencia ni calibración analógica; la entrada ya es cruda.
 */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/misc/esp32c3_saradc.h"

/* Estado y registros independientes del SENS clásico. */
#define C3_ADC_ONETIME 0x20
#define C3_ADC_DATA1 0x2c
#define C3_ADC_ENA 0x40
#define C3_ADC_RAW 0x44
#define C3_ADC_ST 0x48
#define C3_ADC_CLR 0x4c
#define C3_ADC_START (1u << 29)
#define C3_ADC_ENABLE1 (1u << 31)
#define C3_ADC_DONE1 (1u << 31)

void esp32c3_saradc_fijar_valor(ESP32C3SarAdcState *s, uint8_t gpio, uint16_t valor)
{
    if (gpio < ESP32C3_SARADC_CANALES) {
        s->valores[gpio] = MIN(valor, 4095);
    }
}

static void esp32c3_saradc_actualizar_irq(ESP32C3SarAdcState *s)
{
    s->registros[C3_ADC_ST / 4] = s->registros[C3_ADC_RAW / 4] &
                                 s->registros[C3_ADC_ENA / 4];
    qemu_set_irq(s->irq, s->registros[C3_ADC_ST / 4] != 0);
}

static uint64_t esp32c3_saradc_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(opaque);

    return !(addr & 3) && addr < ESP32C3_SARADC_REGS_SIZE ?
           s->registros[addr / 4] : 0;
}

static void esp32c3_saradc_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(opaque);
    uint32_t v = value;

    if ((addr & 3) || addr >= ESP32C3_SARADC_REGS_SIZE) {
        return;
    }
    switch (addr) {
    case 0x10: /* Estado de SAR1/SAR2 y resultados de solo lectura. */
    case 0x14:
    case C3_ADC_DATA1:
    case 0x30:
    case C3_ADC_RAW:
    case C3_ADC_ST:
        return;
    case C3_ADC_CLR:
        s->registros[C3_ADC_RAW / 4] &= ~v;
        break;
    case C3_ADC_ONETIME:
        if ((v & C3_ADC_START) &&
            !(s->registros[addr / 4] & C3_ADC_START) && (v & C3_ADC_ENABLE1)) {
            unsigned canal = (v >> 25) & 15;

            s->registros[C3_ADC_DATA1 / 4] = canal < ESP32C3_SARADC_CANALES ?
                                            s->valores[canal] : 0;
            s->registros[C3_ADC_RAW / 4] |= C3_ADC_DONE1;
        }
        s->registros[addr / 4] = v;
        break;
    default:
        s->registros[addr / 4] = v;
        break;
    }
    esp32c3_saradc_actualizar_irq(s);
}

static const MemoryRegionOps esp32c3_saradc_ops = {
    .read = esp32c3_saradc_read,
    .write = esp32c3_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_saradc_reiniciar(Object *obj, ResetType tipo)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(obj);

    memset(s->registros, 0, sizeof(s->registros));
    esp32c3_saradc_actualizar_irq(s);
    /* Las señales externas se conservan durante el reset. */
}

static void esp32c3_saradc_init(Object *obj)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_saradc_ops, s,
                          TYPE_ESP32C3_SARADC, ESP32C3_SARADC_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void esp32c3_saradc_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_saradc_reiniciar;
}

static const TypeInfo esp32c3_saradc_info = {
    .name = TYPE_ESP32C3_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3SarAdcState),
    .instance_init = esp32c3_saradc_init,
    .class_init = esp32c3_saradc_class_init,
};

static void esp32c3_saradc_register_types(void)
{
    type_register_static(&esp32c3_saradc_info);
}

type_init(esp32c3_saradc_register_types)
