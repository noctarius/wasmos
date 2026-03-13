#![no_std]
#![no_main]

use core::panic::PanicInfo;
#[path = "../../../lib/libc/rust/wasmos.rs"]
mod wasmos;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

fn main(_args: &[&str]) -> i32 {
    static mut PRINTED: bool = false;

    unsafe {
        if !PRINTED {
            PRINTED = true;
            let _ = wasmos::std::puts(b"Hello from Rust on WASMOS!\n");
            let _ = wasmos::std::puts(b"This is a tiny WASMOS-APP written in Rust.\n");
            let _ = wasmos::std::printf(format_args!("Entry: {}\n", "main"));
        }
    }

    0
}
