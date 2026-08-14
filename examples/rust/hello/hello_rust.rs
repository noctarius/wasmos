//! WASMOS "hello world" for the Rust guest binding.
//!
//! Demonstrates console output (`wasmos::std::puts` / `printf`) and the async
//! filesystem API from `wasmos::coroutine`, which is the part worth studying.
//!
//! Every `*_async` call returns an `AsyncFsOp`; `then` chains the next step onto
//! it and yields a `*mut Future`, and `catch` handles a rejection. Returning
//! that future from a step is what schedules the next one, and
//! `coroutine::run_async_app` drives the whole chain to completion and returns
//! the app's exit status. Returning a null future ends the chain, which is what
//! the `fail_*` helpers do after printing their diagnostic.
//!
//! The chain reads one byte of `/boot/startup.nsh`, then creates a file with a
//! long (non-8.3) name, writes to it, reads it back for comparison, unlinks it,
//! and finally stats the removed path expecting that stat to reject — the
//! rejection path is where the success lines are printed.
//!
//! `#![no_std]` and `extern "C"` callbacks cannot capture, so the fd and the
//! read buffers threaded through the chain live in the `APP` singleton.
//!
//! Preconditions: `/boot/startup.nsh` must exist and be at least one byte, and
//! the working directory must be writable — the test file is created with a
//! relative path.
#![no_std]
#![no_main]

use core::panic::PanicInfo;
#[path = "../../../src/libc/rust/wasmos.rs"]
mod wasmos;

use wasmos::coroutine::{self, AsyncFsOp, Future};
use wasmos::{O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY};

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

const PATH: &str = "rust-long-file-check.txt";
const CONTENT: &[u8] = b"rust shim long filename\n";

// The promise callbacks are `extern "C"` function pointers and therefore cannot
// capture, so the small amount of state threaded through the chain (the active
// fd and the read scratch buffers) lives in this app-owned singleton.
struct App {
    fd: i32,
    startup: [u8; 1],
    check: [u8; 32],
}
static mut APP: App = App {
    fd: -1,
    startup: [0u8; 1],
    check: [0u8; 32],
};

fn null_future() -> *mut Future {
    core::ptr::null_mut()
}

fn fail_startup() -> *mut Future {
    let _ = wasmos::std::puts(b"startup.nsh readable: false\n");
    null_future()
}

fn fail_write() -> *mut Future {
    let _ = wasmos::std::puts(b"long filename write: false\n");
    null_future()
}

fn fail_unlink() -> *mut Future {
    let _ = wasmos::std::puts(b"long filename unlink: false\n");
    null_future()
}

extern "C" fn app_start() -> *mut Future {
    match coroutine::open_async("/boot/startup.nsh", O_RDONLY) {
        Some(op) => op.then(startup_opened),
        None => fail_startup(),
    }
}

extern "C" fn startup_opened(open: &mut AsyncFsOp) -> *mut Future {
    let fd = open.result();
    if fd < 0 {
        return fail_startup();
    }
    unsafe { APP.fd = fd };
    let dst = unsafe { core::ptr::addr_of_mut!(APP.startup) as *mut u8 };
    match coroutine::read_async(fd, dst, 1) {
        Some(op) => op.then(startup_read),
        None => fail_startup(),
    }
}

extern "C" fn startup_read(read: &mut AsyncFsOp) -> *mut Future {
    if read.result() != 1 {
        return fail_startup();
    }
    match coroutine::close_async(unsafe { APP.fd }) {
        Some(op) => op.then(startup_closed),
        None => fail_startup(),
    }
}

extern "C" fn startup_closed(close: &mut AsyncFsOp) -> *mut Future {
    if close.result() < 0 {
        return fail_startup();
    }
    let _ = wasmos::std::puts(b"startup.nsh readable: true\n");
    match coroutine::open_async(PATH, O_WRONLY | O_CREAT | O_TRUNC) {
        Some(op) => op.then(file_created),
        None => fail_write(),
    }
}

extern "C" fn file_created(create: &mut AsyncFsOp) -> *mut Future {
    let fd = create.result();
    if fd < 0 {
        return fail_write();
    }
    unsafe { APP.fd = fd };
    match coroutine::write_async(fd, CONTENT.as_ptr(), CONTENT.len() as i32) {
        Some(op) => op.then(file_written),
        None => fail_write(),
    }
}

extern "C" fn file_written(write: &mut AsyncFsOp) -> *mut Future {
    if write.result() != CONTENT.len() as i32 {
        return fail_write();
    }
    match coroutine::close_async(unsafe { APP.fd }) {
        Some(op) => op.then(write_closed),
        None => fail_write(),
    }
}

extern "C" fn write_closed(close: &mut AsyncFsOp) -> *mut Future {
    if close.result() < 0 {
        return fail_write();
    }
    match coroutine::open_async(PATH, O_RDONLY) {
        Some(op) => op.then(verify_opened),
        None => fail_write(),
    }
}

extern "C" fn verify_opened(open: &mut AsyncFsOp) -> *mut Future {
    let fd = open.result();
    if fd < 0 {
        return fail_write();
    }
    unsafe { APP.fd = fd };
    let dst = unsafe { core::ptr::addr_of_mut!(APP.check) as *mut u8 };
    match coroutine::read_async(fd, dst, 32) {
        Some(op) => op.then(verify_read),
        None => fail_write(),
    }
}

extern "C" fn verify_read(read: &mut AsyncFsOp) -> *mut Future {
    let matched = read.result() == CONTENT.len() as i32
        && unsafe {
            core::slice::from_raw_parts(core::ptr::addr_of!(APP.check) as *const u8, CONTENT.len())
        } == CONTENT;
    if !matched {
        return fail_write();
    }
    match coroutine::close_async(unsafe { APP.fd }) {
        Some(op) => op.then(verify_closed),
        None => fail_write(),
    }
}

extern "C" fn verify_closed(close: &mut AsyncFsOp) -> *mut Future {
    if close.result() < 0 {
        return fail_write();
    }
    match coroutine::unlink_async(PATH) {
        Some(op) => op.then(file_unlinked),
        None => fail_unlink(),
    }
}

extern "C" fn file_unlinked(unlink: &mut AsyncFsOp) -> *mut Future {
    if unlink.result() < 0 {
        return fail_unlink();
    }
    // A stat of the just-unlinked path is expected to reject; catch converts
    // that rejection into the success report.
    match coroutine::stat_async(PATH) {
        Some(op) => op.catch(stat_rejected),
        None => fail_unlink(),
    }
}

extern "C" fn stat_rejected(_status: i32) -> i32 {
    let _ = wasmos::std::puts(b"long filename write: true\n");
    let _ = wasmos::std::puts(b"long filename unlink: true\n");
    0
}

fn main(_args: &[&str]) -> i32 {
    let _ = wasmos::std::puts(b"Hello from Rust on WASMOS!\n");
    let _ = wasmos::std::puts(b"This is a tiny WASMOS-APP written in Rust.\n");
    let _ = wasmos::std::printf(format_args!("Entry: {}\n", "main"));
    coroutine::run_async_app(app_start)
}
