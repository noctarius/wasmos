const wasmos = @import("wasmos.zig");

pub fn main() u8 {
    const args = wasmos.cliArgs();
    if (args.len == 0) {
        wasmos.stdlib.write("usage: cat <path>\n") catch {};
        return 1;
    }

    var file = wasmos.fs.openRead(args[0]) catch {
        wasmos.stdlib.write("fs failed\n") catch {};
        return 1;
    };
    defer file.close() catch {};

    var buf: [128]u8 = undefined;
    while (true) {
        const n = file.read(buf[0..]) catch break;
        if (n == 0) break;
        wasmos.stdlib.write(buf[0..n]) catch {};
    }

    return 0;
}
