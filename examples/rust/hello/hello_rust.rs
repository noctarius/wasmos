#![no_std]
#![no_main]

use core::ffi::c_void;
use core::panic::PanicInfo;
#[path = "../../../src/libc/rust/wasmos.rs"]
mod wasmos;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

struct TaskState {
    phase: u32,
}
unsafe extern "C" fn resume_task(user: *mut c_void, out: *mut usize) -> i32 {
    let state = unsafe { &mut *(user as *mut TaskState) };
    if state.phase == 0 {
        state.phase = 1;
        wasmos::coroutine::TaskResult::YIELDED
    } else {
        unsafe { *out = 42 };
        wasmos::coroutine::TaskResult::COMPLETE
    }
}

fn main(_args: &[&str]) -> i32 {
    static mut PRINTED: bool = false;
    const PATH: &str = "rust-long-file-check.txt";
    const CONTENT: &[u8] = b"rust shim long filename\n";
    unsafe {
        if !PRINTED {
            let mut file_ok = false;
            if let Ok(file) = wasmos::fs::create(PATH) {
                if file.write(CONTENT).ok() == Some(CONTENT.len()) && file.close().is_ok() {
                    let mut buffer = [0u8; 32];
                    if let Ok(read_file) = wasmos::fs::open_read(PATH) {
                        if read_file.read(&mut buffer).ok() == Some(CONTENT.len())
                            && read_file.close().is_ok()
                        {
                            file_ok = &buffer[..CONTENT.len()] == CONTENT;
                        }
                    }
                }
            }
            let mut runtime = wasmos::coroutine::Runtime::new();
            let mut coroutine = wasmos::coroutine::Coroutine::new();
            let mut state = TaskState { phase: 0 };
            runtime.init();
            let coroutine_ok = coroutine
                .start(
                    &mut runtime,
                    resume_task,
                    &mut state as *mut _ as *mut c_void,
                )
                .is_some()
                && runtime.run().is_ok()
                && matches!(coroutine.join(), Ok(42));
            PRINTED = true;
            let _ = wasmos::std::puts(b"Hello from Rust on WASMOS!\n");
            let _ = wasmos::std::puts(b"This is a tiny WASMOS-APP written in Rust.\n");
            let _ = wasmos::std::printf(format_args!("Entry: {}\n", "main"));
            let _ = wasmos::std::printf(format_args!("long filename write: {}\n", file_ok));
            let unlink_ok = if file_ok {
                wasmos::fs::unlink(PATH).is_ok() && wasmos::fs::stat(PATH).is_err()
            } else {
                false
            };
            let _ = wasmos::std::printf(format_args!("long filename unlink: {}\n", unlink_ok));
            let _ = wasmos::std::puts(if coroutine_ok {
                b"coroutine task: ready\n"
            } else {
                b"coroutine task: failed\n"
            });
        }
    }
    0
}
