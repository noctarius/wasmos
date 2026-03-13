const wasmos = @import("wasmos.zig");

var printed: bool = false;

pub fn main() void {}

pub export fn wasmos_entry(
    ignored_arg0: i32,
    ignored_arg1: i32,
    ignored_arg2: i32,
    ignored_arg3: i32,
) callconv(.c) void {
    _ = ignored_arg0;
    _ = ignored_arg1;
    _ = ignored_arg2;
    _ = ignored_arg3;

    if (!printed) {
        printed = true;
        _ = wasmos.putsn("Hello from Zig on WASMOS!\n");
        _ = wasmos.putsn("This is a tiny WASMOS-APP written in Zig.\n");
        _ = wasmos.putsn("Entry: main\n");
    }
}
