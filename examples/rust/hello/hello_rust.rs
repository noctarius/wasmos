#![no_std]
#![no_main]

use core::panic::PanicInfo;

#[link(wasm_import_module = "wasmos")]
extern "C" {
    fn console_write(ptr: i32, len: i32) -> i32;
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[inline]
fn write_line(bytes: &[u8]) {
    if !bytes.is_empty() {
        unsafe {
            console_write(bytes.as_ptr() as i32, bytes.len() as i32);
        }
    }
}

#[no_mangle]
pub extern "C" fn main(
    _arg0: i32,
    _arg1: i32,
    _arg2: i32,
    _arg3: i32,
) -> i32 {
    static mut PRINTED: bool = false;

    unsafe {
        if !PRINTED {
            PRINTED = true;
            write_line(b"Hello from Rust on WASMOS!\n");
            write_line(b"This is a tiny WASMOS-APP written in Rust.\n");
            write_line(b"Entry: main\n");
        }
    }

    0
}
