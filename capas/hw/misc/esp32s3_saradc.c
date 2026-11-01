/* ADC1 S3 de una lectura por software, sobre SENS (no APB_SARADC).
 * Copyright (c) 2026 Taller. GPL-2.0-or-later.
 * Registros y secuencia: sens_reg.h y adc_ll.h de Arduino ESP32 3.3.12.
 * ADC2 solo termina con cero para la autocalibración de arranque de IDF.
 * Sin canales ADC2 externos, DMA, latencia, ULP, IRQ ni calibración analógica.
 */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_saradc.h"

/* Estado y registros independientes del APB_SARADC C3 y del SENS clásico. */
#define S3_ADC_MEAS1 0x0c
#define S3_ADC_MEAS2 0x30
#define S3_ADC_SLAVE1 0x40
#define S3_ADC_START (1u << 17)
#define S3_ADC_DONE (1u << 16)
#define S3_ADC_DATO 0xffffu

static bool esp32s3_saradc_traza(void)
{
    static int activa = -1;

    if (activa < 0) {
        const char *valor = getenv("TALLER_ADC_TRAZA");

        activa = valor && !strcmp(valor, "1");
    }
    return activa;
}

void esp32s3_saradc_fijar_valor(ESP32S3SarAdcState *s, uint8_t gpio, uint16_t valor)
{
    if (gpio >= 1 && gpio <= ESP32S3_SARADC_CANALES) {
        s->valores[gpio - 1] = MIN(valor, 4095);
    }
}

static uint64_t esp32s3_saradc_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3SarAdcState *s = ESP32S3_SARADC(opaque);
    uint32_t valor = !(addr & 3) && addr < ESP32S3_SARADC_REGS_SIZE ?
                     s->registros[addr / 4] : 0;

    if (esp32s3_saradc_traza()) {
        fprintf(stderr, "adc-s3 SENS L %08x/%u = %08x\n",
                0x60008800u + (unsigned)addr, size, valor);
    }
    return valor;
}

static void esp32s3_saradc_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    ESP32S3SarAdcState *s = ESP32S3_SARADC(opaque);
    uint32_t v = value;

    if (esp32s3_saradc_traza()) {
        fprintf(stderr, "adc-s3 SENS E %08x/%u = %08x\n",
                0x60008800u + (unsigned)addr, size, v);
    }
    if ((addr & 3) || addr >= ESP32S3_SARADC_REGS_SIZE) {
        return;
    }
    if (addr == S3_ADC_MEAS1 || addr == S3_ADC_MEAS2) {
        uint32_t anterior = s->registros[addr / 4];
        uint32_t resultado = anterior & (S3_ADC_DONE | S3_ADC_DATO);

        if (!(v & S3_ADC_START)) {
            resultado &= ~S3_ADC_DONE;
        } else if (!(anterior & S3_ADC_START)) {
            unsigned pads = (v >> 19) & 0xfff;

            resultado = S3_ADC_DONE;
            /* adc_hw_calibration() calibra AMBAS unidades antes de setup(),
             * aun usando solo analogRead(1). Sin eFuses, read_cal_channel()
             * pulsa START sin pads y espera DONE en MEAS2 también.
             * ADC2 devuelve cero (GND); no tiene entradas externas modeladas.
             */
            if (addr == S3_ADC_MEAS1) {
                /* El LL selecciona un pad; sin pad (calibración) da cero. */
                for (unsigned canal = 0; canal < ESP32S3_SARADC_CANALES; canal++) {
                    if (pads == (1u << canal)) {
                        resultado |= s->valores[canal];
                        break;
                    }
                }
            }
        }
        s->registros[addr / 4] = (v & ~(S3_ADC_DONE | S3_ADC_DATO)) | resultado;
    } else if (addr == S3_ADC_SLAVE1) {
        s->registros[addr / 4] = v & ~(0xffu << 22); /* MEAS_STATUS: libre. */
    } else {
        s->registros[addr / 4] = v;
    }
}

static const MemoryRegionOps esp32s3_saradc_ops = {
    .read = esp32s3_saradc_read,
    .write = esp32s3_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32s3_saradc_reiniciar(Object *obj, ResetType tipo)
{
    ESP32S3SarAdcState *s = ESP32S3_SARADC(obj);

    memset(s->registros, 0, sizeof(s->registros));
    /* Las entradas externas sobreviven al reset, como en C3. */
}

static void esp32s3_saradc_init(Object *obj)
{
    ESP32S3SarAdcState *s = ESP32S3_SARADC(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_saradc_ops, s,
                          TYPE_ESP32S3_SARADC, ESP32S3_SARADC_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void esp32s3_saradc_class_init(ObjectClass *klass, void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = esp32s3_saradc_reiniciar;
}

static const TypeInfo esp32s3_saradc_info = {
    .name = TYPE_ESP32S3_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3SarAdcState),
    .instance_init = esp32s3_saradc_init,
    .class_init = esp32s3_saradc_class_init,
};

static void esp32s3_saradc_register_types(void)
{
    type_register_static(&esp32s3_saradc_info);
}

type_init(esp32s3_saradc_register_types)
