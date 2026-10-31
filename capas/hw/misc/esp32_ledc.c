#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/misc/esp32_ledc.h"
#include "hw/gpio/esp32_gpio.h"
#include "hw/misc/taller_puente.h"

#define ESP32_LEDC_REGS_SIZE (A_LEDC_CONF_REG + 4)

/* Igual que GPIO, hay un LEDC por ESP32; la matriz puede cambiar después del duty. */
static Esp32LEDCState *ledc_del_chip;

static uint32_t *esp32_ledc_registro(Esp32LEDCState *s, hwaddr addr)
{
    if (addr & 3) {
        return NULL;
    }
    if (addr < A_LEDC_HSTIMER0_CONF_REG) {
        unsigned canal = (unsigned)(addr / 0x14);

        switch (addr % 0x14) {
        case 0x00: return &s->channel_conf0_reg[canal];
        case 0x04: return &s->canal_hpoint[canal];
        case 0x08: return &s->canal_duty[canal];
        case 0x0c: return &s->canal_conf1[canal];
        case 0x10: return &s->canal_duty_leido[canal];
        }
    } else if (addr < A_LEDC_INT_RAW_REG) {
        unsigned temporizador = (unsigned)((addr - A_LEDC_HSTIMER0_CONF_REG) / 8);

        return (addr & 4) ? &s->timer_valor[temporizador] :
                           &s->timer_conf_reg[temporizador];
    } else if (addr <= A_LEDC_CONF_REG) {
        return &s->registros_comunes[(addr - A_LEDC_INT_RAW_REG) / 4];
    }
    return NULL;
}

static uint64_t esp32_ledc_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32LEDCState *s = ESP32_LEDC(opaque);
    uint32_t *registro = esp32_ledc_registro(s, addr);
    uint32_t valor = registro ? *registro : 0;

    if (registro && addr < A_LEDC_HSTIMER0_CONF_REG &&
        addr % 0x14 == A_LEDC_HSCH0_CONF1_REG) {
        /* El cambio es inmediato: la siguiente lectura ya no debe esperar al
         * hardware. El core 3.x sondea DUTY_START antes de actualizar el duty. */
        *registro &= ~R_LEDC_HSCH0_CONF1_REG_DUTY_START_MASK;
    }
    return valor;
}

static void esp32_ledc_actualizar_canal(Esp32LEDCState *s, unsigned canal)
{
    uint32_t conf0 = s->channel_conf0_reg[canal];
    unsigned temporizador = FIELD_EX32(conf0, LEDC_HSCH0_CONF0_REG, TIMER_SEL) +
                             (canal < 8 ? 0 : 4);
    uint32_t conf_timer = s->timer_conf_reg[temporizador];
    unsigned resolucion = s->duty_res[temporizador];
    uint32_t divisor = FIELD_EX32(conf_timer, LEDC_HSTIMER0_CONF_REG, DIV_NUM);
    uint32_t frecuencia_mhz = 0;
    uint32_t ciclo_ppm = 0;
    bool habilitado = FIELD_EX32(conf0, LEDC_HSCH0_CONF0_REG, SIG_OUT_EN);
    int senal = canal < 8 ? ESP32_LEDC_SENAL_HS0 + canal :
                           ESP32_LEDC_SENAL_LS0 + canal - 8;
    int gpio = esp32_gpio_pin_de_senal(senal);

    if (divisor && resolucion > 0 && resolucion <= 20) {
        uint64_t periodo = 1ULL << resolucion;
        uint64_t reloj = FIELD_EX32(conf_timer, LEDC_HSTIMER0_CONF_REG, TICK_SEL) ?
                         80000000 : 1000000;
        uint64_t denominador = divisor * periodo;
        uint64_t frecuencia = (reloj * 256 * 1000 + denominador / 2) / denominador;
        uint32_t duty = FIELD_EX32(s->canal_duty[canal],
                                  LEDC_HSCH0_DUTY_REG, DUTY) >> 4;

        /* DIV_NUM tiene ocho bits fraccionarios; los cuatro de DUTY se omiten.
         * Usar 64 bits evita desbordar incluso con resolución de veinte bits. */
        frecuencia_mhz = (uint32_t)MIN(frecuencia, UINT32_MAX);
        if (habilitado) {
            ciclo_ppm = (uint32_t)MIN((uint64_t)duty * 1000000 / periodo, 1000000);
        }
    }
    /* Conservar los LED del dispositivo original, además del puente de Taller. */
    led_set_intensity(&s->led[canal], ciclo_ppm / 10000);

    if (s->pwm_emitido[canal] && s->pwm_gpio[canal] != gpio) {
        /* El receptor conserva el último PWM: apagar la ruta que desaparece. */
        taller_puente_pwm(s->pwm_gpio[canal], s->pwm_frecuencia_mhz[canal], 0);
        s->pwm_emitido[canal] = false;
    }
    if (gpio < 0 || (!habilitado && !s->pwm_emitido[canal])) {
        return;
    }
    if (!s->pwm_emitido[canal] || s->pwm_gpio[canal] != gpio ||
        s->pwm_frecuencia_mhz[canal] != frecuencia_mhz ||
        s->pwm_ciclo_ppm[canal] != ciclo_ppm) {
        taller_puente_pwm(gpio, frecuencia_mhz, ciclo_ppm);
        s->pwm_emitido[canal] = true;
        s->pwm_gpio[canal] = gpio;
        s->pwm_frecuencia_mhz[canal] = frecuencia_mhz;
        s->pwm_ciclo_ppm[canal] = ciclo_ppm;
    }
}

void esp32_ledc_actualizar_senal(int senal)
{
    if (!ledc_del_chip) {
        return;
    }
    if (senal >= ESP32_LEDC_SENAL_HS0 && senal < ESP32_LEDC_SENAL_HS0 + 8) {
        esp32_ledc_actualizar_canal(ledc_del_chip, senal - ESP32_LEDC_SENAL_HS0);
    } else if (senal >= ESP32_LEDC_SENAL_LS0 && senal < ESP32_LEDC_SENAL_LS0 + 8) {
        esp32_ledc_actualizar_canal(ledc_del_chip, senal - ESP32_LEDC_SENAL_LS0 + 8);
    }
}

static void esp32_ledc_write(void *opaque, hwaddr addr,
                            uint64_t value, unsigned int size)
{
    Esp32LEDCState *s = ESP32_LEDC(opaque);
    uint32_t *registro = esp32_ledc_registro(s, addr);

    if (!registro) {
        return;
    }
    *registro = (uint32_t)value;
    if (addr < A_LEDC_HSTIMER0_CONF_REG) {
        unsigned canal = (unsigned)(addr / 0x14);

        if (addr % 0x14 == A_LEDC_HSCH0_DUTY_REG) {
            /* Sin fades, DUTY_R refleja el duty aplicado inmediatamente. */
            s->canal_duty_leido[canal] = value & R_LEDC_HSCH0_DUTY_REG_DUTY_MASK;
        }
        /* Incluye PARA_UP de LS. Se tolera que falte el pulso de actualización;
         * la caché evita repetir tramas durante las escrituras sucesivas del SDK. */
        esp32_ledc_actualizar_canal(s, canal);
    } else if (addr < A_LEDC_INT_RAW_REG && !(addr & 4)) {
        unsigned temporizador = (unsigned)((addr - A_LEDC_HSTIMER0_CONF_REG) / 8);
        unsigned primero = temporizador < 4 ? 0 : 8;

        s->duty_res[temporizador] = FIELD_EX32(value, LEDC_HSTIMER0_CONF_REG, DUTY_RES);
        for (unsigned canal = primero; canal < primero + 8; canal++) {
            if (FIELD_EX32(s->channel_conf0_reg[canal],
                           LEDC_HSCH0_CONF0_REG, TIMER_SEL) == temporizador % 4) {
                esp32_ledc_actualizar_canal(s, canal);
            }
        }
    }
}

static const MemoryRegionOps esp32_ledc_ops = {
        .read =  esp32_ledc_read,
        .write = esp32_ledc_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid.min_access_size = 4,
        .valid.max_access_size = 4,
        .impl.min_access_size = 4,
        .impl.max_access_size = 4,
};

static void esp32_ledc_reiniciar(Object *obj, ResetType tipo)
{
    Esp32LEDCState *s = ESP32_LEDC(obj);

    for (int canal = 0; canal < ESP32_LEDC_CHANNEL_CNT; canal++) {
        if (s->pwm_emitido[canal]) {
            taller_puente_pwm(s->pwm_gpio[canal], s->pwm_frecuencia_mhz[canal], 0);
        }
        s->pwm_emitido[canal] = false;
        s->pwm_gpio[canal] = -1;
        s->pwm_frecuencia_mhz[canal] = 0;
        s->pwm_ciclo_ppm[canal] = 0;
        s->channel_conf0_reg[canal] = 0;
        s->canal_hpoint[canal] = 0;
        s->canal_duty[canal] = 0;
        s->canal_conf1[canal] = 0;
        s->canal_duty_leido[canal] = 0;
        led_set_intensity(&s->led[canal], 0);
    }
    memset(s->duty_res, 0, sizeof(s->duty_res));
    memset(s->timer_conf_reg, 0, sizeof(s->timer_conf_reg));
    memset(s->timer_valor, 0, sizeof(s->timer_valor));
    memset(s->registros_comunes, 0, sizeof(s->registros_comunes));
}

static void esp32_ledc_realize(DeviceState *dev, Error **errp)
{
    Esp32LEDCState *s = ESP32_LEDC(dev);
    for (int i = 0; i < ESP32_LEDC_CHANNEL_CNT; i++) {
        qdev_realize(DEVICE(&s->led[i]), NULL, &error_fatal);
    }
}

static void esp32_ledc_init(Object *obj)
{
    Esp32LEDCState *s = ESP32_LEDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    ledc_del_chip = s;
    memory_region_init_io(&s->iomem, obj, &esp32_ledc_ops, s,
                          TYPE_ESP32_LEDC, ESP32_LEDC_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (int i = 0; i < ESP32_LEDC_CHANNEL_CNT; i++) {
        object_initialize_child(obj, g_strdup_printf("led%d", i + 1), &s->led[i], TYPE_LED);
        s->led[i].color = (char *)"blue";
    }
}

static void esp32_ledc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_ledc_reiniciar;
    dc->realize = esp32_ledc_realize;
}

static const TypeInfo esp32_ledc_info = {
        .name = TYPE_ESP32_LEDC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Esp32LEDCState),
        .instance_init = esp32_ledc_init,
        .class_init = esp32_ledc_class_init
};

static void esp32_ledc_register_types(void)
{
    type_register_static(&esp32_ledc_info);
}

type_init(esp32_ledc_register_types)
