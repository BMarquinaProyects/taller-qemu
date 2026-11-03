// Prueba de regresión de M-160 (esp-hal 1.2, sin ESP-IDF), para el ESP32 y el ESP32-S3.
//
// esp-hal no escribe nunca CPENABLE: confía en que el silicio salga del reset con la FPU
// habilitada (0xff). Si QEMU la deja a 0, la primera instrucción de coma flotante lanza la
// excepción de coprocesador y el manejador de xtensa-lx-rt (float-save-restore) acaba en
// doble excepción. En el S3, además, esp_hal::init espera a BBPLL_CAL_DONE (bit 24 de
// I2C_MST_ANA_CONF0, 0x6000E040).
//
// Salida esperada por la UART0:
//   TALLER fpu cpenable=0xff
//   TALLER-OK fpu 105 -21 3
#![no_std]
#![no_main]
#![feature(asm_experimental_arch)]

use core::hint::black_box;
use esp_backtrace as _;
use esp_hal::main;
use esp_println::println;

esp_bootloader_esp_idf::esp_app_desc!();

fn cpenable() -> u32 {
    let v: u32;
    unsafe { core::arch::asm!("rsr.cpenable {0}", out(reg) v) };
    v
}

#[main]
fn main() -> ! {
    // Antes de esp_hal::init: el valor que deja el reset (la ROM y el bootloader no lo tocan).
    let al_arrancar = cpenable();
    let _perifericos = esp_hal::init(esp_hal::Config::default());
    println!("TALLER fpu cpenable={:#x}", al_arrancar);
    // black_box impide que el compilador calcule las operaciones al compilar.
    let x = black_box(7u32) as f32 * black_box(1.5f32);         // ufloat.s y mul.s
    let y = black_box(-3.5f32) * black_box(6.0f32);             // -21
    let z = black_box(10.0f32) / black_box(3.0f32);             // div0.s/divn.s (secuencia)
    println!("TALLER-OK fpu {} {} {}", (x * 10.0) as u32, y as i32, z as i32);
    loop {}
}
