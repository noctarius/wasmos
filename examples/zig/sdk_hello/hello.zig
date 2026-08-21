//! sdk_hello (Zig) - the Zig driver's own smoke app.
//!
//! Plain Zig against the `wasmos.zig` shim, built by the staged SDK
//! (cmake/WasmosSdk.cmake) with no flags but -o and no linker.metadata of its
//! own. It exercises what a developer never sees on the Zig path: the shims
//! staged flat so `@import("wasmos.zig")` resolves, the mandatory 8 KiB shadow
//! stack, the layout check that refuses a module whose globals would sit above
//! the kernel's user-VA mirror region, and the default manifest.
//!
//! Deliberately minimal. The Zig binding's breadth is covered by
//! examples/zig/hello; what is under test here is the driver.
const wasmos = @import("wasmos.zig");

pub fn main() u8 {
    wasmos.stdlib.println("Hello WASMOS from Zig via the SDK!", .{}) catch return 1;
    return 0;
}
