//! sdk_hello (Rust) - the Rust driver's own smoke app.
//!
//! Plain `#![no_std]` Rust against the `wasmos` binding, built by the staged SDK
//! (cmake/WasmosSdk.cmake) with no flags but -o and no linker.metadata of its own.
//!
//! `mod wasmos;` is the whole point: in tree an app reaches the binding with an
//! `#[path = "../../../src/libc/rust/wasmos.rs"]` escape, which an out-of-tree app
//! cannot write. The driver stages the binding as a sibling module instead --
//! wasmos.rs beside the app, and its own `pub mod coroutine;` child under
//! wasmos/ -- so a plain declaration resolves.
//!
//! Deliberately minimal. The Rust binding's breadth is covered by
//! examples/rust/hello; what is under test here is the driver.
#![no_std]
#![no_main]

use core::panic::PanicInfo;
mod wasmos;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

/// The binding's `wasmos_main` export calls this as `crate::main`. That is a plain
/// Rust call, not an FFI boundary, so the signature carries no `extern "C"` and no
/// `#[no_mangle]`: `&[&str]` has no C representation, and claiming the C ABI for it
/// is what `improper_ctypes_definitions` rejects. `args` is empty because the Rust
/// shim does not tokenize the spawn-info argument string yet (see docs/TASKS.md).
fn main(_args: &[&str]) -> i32 {
    let _ = wasmos::std::puts(b"Hello WASMOS from Rust via the SDK!\n");
    0
}
