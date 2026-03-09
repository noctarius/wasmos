const WASMOS_WASM_STEP_YIELDED: i32 = 0;

extern "wasmos" fn console_write(ptr: i32, len: i32) callconv(.c) i32;

var printed: bool = false;

fn write_line(s: []const u8) void {
    if (s.len == 0) {
        return;
    }
    const ptr: i32 = @intCast(@intFromPtr(s.ptr));
    const len: i32 = @intCast(s.len);
    _ = console_write(ptr, len);
}

pub export fn hello_zig_step(
    ignored_type: i32,
    ignored_arg0: i32,
    ignored_arg1: i32,
    ignored_arg2: i32,
    ignored_arg3: i32,
) callconv(.c) i32 {
    _ = ignored_type;
    _ = ignored_arg0;
    _ = ignored_arg1;
    _ = ignored_arg2;
    _ = ignored_arg3;

    if (!printed) {
        printed = true;
        write_line("Hello from Zig on WASMOS!\n");
        write_line("This is a tiny WASMOS-APP written in Zig.\n");
        write_line("Entry: hello_zig_step\n");
    }

    return WASMOS_WASM_STEP_YIELDED;
}
