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
    const PATH: &str = "/rust-long-file-check.txt";
    const CONTENT: &[u8] = b"rust shim long filename\n";

    unsafe {
        if !PRINTED {
            let mut file_ok = false;

            if let Ok(file) = wasmos::fs::create(PATH) {
                if file.write(CONTENT).ok() == Some(CONTENT.len()) && file.close().is_ok() {
                    let mut buffer = [0u8; 32];
                    if let Ok(read_file) = wasmos::fs::open_read(PATH) {
                        if read_file.read(&mut buffer).ok() == Some(CONTENT.len()) && read_file.close().is_ok() {
                            file_ok = &buffer[..CONTENT.len()] == CONTENT;
                        }
                    }
                }
            }

            PRINTED = true;
            let _ = wasmos::std::puts(b"Hello from Rust on WASMOS!\n");
            let _ = wasmos::std::puts(b"This is a tiny WASMOS-APP written in Rust.\n");
            let _ = wasmos::std::printf(format_args!("Entry: {}\n", "main"));
            let _ = wasmos::std::printf(format_args!("long filename write: {}\n", file_ok));
        }
    }

    0
}
