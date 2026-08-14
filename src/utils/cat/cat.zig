// cat - write one file to stdout.
//
// Usage: cat <path>
//
// Exactly one argument, a VFS path; extra arguments are ignored and no options
// are recognised, so a leading '-' is treated as part of a filename. The file is
// streamed in 128-byte chunks and written verbatim (no newline is added and no
// text translation is done). Exit status 1 with a usage or "fs failed" line on
// stdout when no argument is given or the file cannot be opened, else 0 — a read
// error part way through ends the copy silently and still exits 0.
const wasmos = @import("wasmos.zig");

/// Program entry point; returns the process exit status.
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
