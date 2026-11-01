/* LEDC C3/S3: cuatro timers y seis u ocho canales LS en el puente de Taller.
 * Registros: soc/ledc_reg.h y hal/ledc_ll.h del core Arduino 3.3.12.
 * GPL-2.0-or-later. No simula fades, IRQ ni pulsos GPIO individuales.
 */
#include "qemu/osdep.h"
#include "hw/misc/esp32c3_ledc.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/misc/taller_puente.h"

/* Estado y registros independientes del LEDC clásico. */
static ESP32C3LEDCState *ledc_del_chip;

static bool esp32c3_ledc_registro_valido(ESP32C3LEDCState *s, hwaddr addr)
{
    return !(addr & 3) && (addr < 0x14 * s->canales ||
           (addr >= 0xa0 && addr <= 0xd0) || addr == 0xfc);
}

static uint64_t esp32c3_ledc_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3LEDCState *s = ESP32C3_LEDC(opaque);

    return esp32c3_ledc_registro_valido(s, addr) ? s->registros[addr / 4] : 0;
}

static void esp32c3_ledc_actualizar_canal(ESP32C3LEDCState *s, unsigned canal)
{
    uint32_t conf = s->registros[canal * 5];
    uint32_t timer = s->registros[(0xa0 + 8 * (conf & 3)) / 4];
    unsigned resolucion = timer & 15;
    uint32_t divisor = (timer >> 4) & 0x3ffff;
    unsigned fuente = s->registros[0xd0 / 4] & 3;
    uint32_t reloj = fuente == 1 ? 80000000 :
                     fuente == 2 ? 17500000 : fuente == 3 ? 40000000 : 0;
    uint32_t frecuencia_mhz = 0, ciclo_ppm = 0;
    bool habilitado = conf & (1u << 2);
    int gpio = s->pin_de_senal ? s->pin_de_senal(s->gpio, s->senal0 + canal) : -1;

    if (reloj && divisor && resolucion > 0 && resolucion <= 14 &&
        !(timer & ((1u << 22) | (1u << 23)))) {
        uint64_t periodo = 1ULL << resolucion;
        uint64_t denominador = divisor * periodo;
        uint64_t frecuencia = ((uint64_t)reloj * 256 * 1000 + denominador / 2) /
                               denominador;
        uint32_t duty = (s->registros[canal * 5 + 2] & 0x7ffff) >> 4;

        frecuencia_mhz = MIN(frecuencia, UINT32_MAX);
        if (habilitado && frecuencia_mhz) {
            ciclo_ppm = MIN((uint64_t)duty * 1000000 / periodo, 1000000);
        }
    }
    if (s->pwm_emitido[canal] && s->pwm_gpio[canal] != gpio) {
        taller_puente_pwm(s->pwm_gpio[canal], s->pwm_frecuencia_mhz[canal], 0);
        s->pwm_emitido[canal] = false;
    }
    if (gpio < 0 || (!habilitado && !s->pwm_emitido[canal])) {
        return;
    }
    if (!s->pwm_emitido[canal] || s->pwm_frecuencia_mhz[canal] != frecuencia_mhz ||
        s->pwm_ciclo_ppm[canal] != ciclo_ppm) {
        taller_puente_pwm(gpio, frecuencia_mhz, ciclo_ppm);
        s->pwm_emitido[canal] = true;
        s->pwm_gpio[canal] = gpio;
        s->pwm_frecuencia_mhz[canal] = frecuencia_mhz;
        s->pwm_ciclo_ppm[canal] = ciclo_ppm;
    }
}

void esp32c3_ledc_actualizar_senal(int senal)
{
    if (ledc_del_chip && senal >= (int)ledc_del_chip->senal0 &&
        (unsigned)senal < ledc_del_chip->senal0 + ledc_del_chip->canales) {
        esp32c3_ledc_actualizar_canal(ledc_del_chip, senal - ledc_del_chip->senal0);
    }
}

static void esp32c3_ledc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    ESP32C3LEDCState *s = ESP32C3_LEDC(opaque);
    uint32_t v = value;

    if (!esp32c3_ledc_registro_valido(s, addr)) {
        return;
    }
    if (addr < 0xa0) {
        unsigned canal = addr / 0x14;
        unsigned campo = addr % 0x14;

        if (campo == 0x10) {
            return; /* DUTY_R es de solo lectura. */
        }
        if (campo == 0) {
            v &= ~(1u << 4); /* PARA_UP: pulso, actualización inmediata. */
        } else if (campo == 0x0c) {
            v &= ~(1u << 31); /* Sin fade: DUTY_START termina sin sondeo ocupado. */
        } else if (campo == 8) {
            v &= 0x7ffff;
            s->registros[canal * 5 + 4] = v;
        }
        s->registros[addr / 4] = v;
        /* Igual que el clásico, admite escrituras anteriores a PARA_UP. */
        esp32c3_ledc_actualizar_canal(s, canal);
    } else if (addr < 0xc0) {
        if (addr & 4) {
            return; /* Contador no temporizado, siempre cero. */
        }
        s->registros[addr / 4] = v & ~(1u << 25);
        for (unsigned canal = 0; canal < s->canales; canal++) {
            if ((s->registros[canal * 5] & 3) == (addr - 0xa0) / 8) {
                esp32c3_ledc_actualizar_canal(s, canal);
            }
        }
    } else if (addr == 0xc8) {
        s->registros[addr / 4] = v; /* Sin eventos ni IRQ de fade/overflow. */
    } else if (addr == 0xd0) {
        s->registros[addr / 4] = v;
        for (unsigned canal = 0; canal < s->canales; canal++) {
            esp32c3_ledc_actualizar_canal(s, canal);
        }
    }
}

static const MemoryRegionOps esp32c3_ledc_ops = {
    .read = esp32c3_ledc_read,
    .write = esp32c3_ledc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_ledc_reiniciar(Object *obj, ResetType tipo)
{
    ESP32C3LEDCState *s = ESP32C3_LEDC(obj);

    for (unsigned canal = 0; canal < s->canales; canal++) {
        if (s->pwm_emitido[canal]) {
            taller_puente_pwm(s->pwm_gpio[canal], s->pwm_frecuencia_mhz[canal], 0);
        }
        s->pwm_emitido[canal] = false;
        s->pwm_gpio[canal] = -1;
        s->pwm_frecuencia_mhz[canal] = s->pwm_ciclo_ppm[canal] = 0;
    }
    memset(s->registros, 0, sizeof(s->registros));
    for (unsigned t = 0; t < ESP32C3_LEDC_TIMERS; t++) {
        s->registros[(0xa0 + 8 * t) / 4] = 1u << 23;
    }
}

static void esp32c3_ledc_init(Object *obj)
{
    ESP32C3LEDCState *s = ESP32C3_LEDC(obj);

    ledc_del_chip = s;
    memory_region_init_io(&s->iomem, obj, &esp32c3_ledc_ops, s,
                          TYPE_ESP32C3_LEDC, ESP32C3_LEDC_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void esp32c3_ledc_realize(DeviceState *dev, Error **errp)
{
    ESP32C3LEDCState *s = ESP32C3_LEDC(dev);

    if (!s->canales || s->canales > ESP32C3_LEDC_CANALES_MAX ||
        s->senal0 > 256 - s->canales) {
        error_setg(errp, "LEDC LS: canales o señales fuera de rango");
    }
}

static Property esp32c3_ledc_properties[] = {
    DEFINE_PROP_UINT32("canales", ESP32C3LEDCState, canales, ESP32C3_LEDC_CANALES),
    DEFINE_PROP_UINT32("senal0", ESP32C3LEDCState, senal0, ESP32C3_LEDC_SENAL0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_ledc_finalize(Object *obj)
{
    if (ledc_del_chip == ESP32C3_LEDC(obj)) {
        ledc_del_chip = NULL;
    }
}

static void esp32c3_ledc_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_ledc_reiniciar;
    DEVICE_CLASS(klass)->realize = esp32c3_ledc_realize;
    device_class_set_props(DEVICE_CLASS(klass), esp32c3_ledc_properties);
}

static const TypeInfo esp32c3_ledc_info = {
    .name = TYPE_ESP32C3_LEDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3LEDCState),
    .instance_init = esp32c3_ledc_init,
    .instance_finalize = esp32c3_ledc_finalize,
    .class_init = esp32c3_ledc_class_init,
};

static void esp32c3_ledc_register_types(void)
{
    type_register_static(&esp32c3_ledc_info);
}

type_init(esp32c3_ledc_register_types)
