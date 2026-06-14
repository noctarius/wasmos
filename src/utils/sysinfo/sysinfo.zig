const std = @import("std");
const wasmos = @import("wasmos.zig");

extern "wasmos" fn physmem_stats(out_ptr: i32) callconv(.c) i32;
extern "wasmos" fn kernel_runtime() callconv(.c) i32;
extern "wasmos" fn sched_cpu_count() callconv(.c) i32;
extern "wasmos" fn sched_ticks() callconv(.c) i32;
extern "wasmos" fn sched_ready_count() callconv(.c) i32;
extern "wasmos" fn proc_count() callconv(.c) i32;
extern "wasmos" fn sync_user_read(ptr: i32, len: i32) callconv(.c) i32;

const PhysmemStats = extern struct {
    total_bytes: u64,
    free_bytes: u64,
};

comptime {
    if (@sizeOf(PhysmemStats) != 16) @compileError("PhysmemStats size mismatch");
    if (@offsetOf(PhysmemStats, "free_bytes") != 8) @compileError("PhysmemStats layout mismatch");
}

var g_physmem: PhysmemStats = undefined;

fn writeStr(s: []const u8) void {
    wasmos.stdlib.write(s) catch {};
}

fn writeLine(buf: []u8, comptime fmt: []const u8, args: anytype) void {
    const s = std.fmt.bufPrint(buf, fmt, args) catch return;
    writeStr(s);
}

fn fmtBytes(bytes: u64, buf: []u8) []const u8 {
    if (bytes >= 1024 * 1024 * 1024) {
        return std.fmt.bufPrint(buf, "{d} GB", .{bytes / (1024 * 1024 * 1024)}) catch "?";
    } else if (bytes >= 1024 * 1024) {
        return std.fmt.bufPrint(buf, "{d} MB", .{bytes / (1024 * 1024)}) catch "?";
    } else if (bytes >= 1024) {
        return std.fmt.bufPrint(buf, "{d} KB", .{bytes / 1024}) catch "?";
    } else {
        return std.fmt.bufPrint(buf, "{d} B", .{bytes}) catch "?";
    }
}

pub fn main() u8 {
    var line: [128]u8 = undefined;

    // --- kernel ---
    writeStr("kernel\n");
    writeStr("  arch:     x86_64\n");

    const rt = kernel_runtime();
    const rt_name: []const u8 = if (rt == 1) "warp (JIT)" else "wasm3 (interpreter)";
    writeLine(&line, "  runtime:  {s}\n", .{rt_name});

    // --- memory ---
    writeStr("\nmemory\n");
    const pm_rc = physmem_stats(@intCast(@intFromPtr(&g_physmem)));
    const ok = pm_rc == 0 and
        sync_user_read(@intCast(@intFromPtr(&g_physmem)), @intCast(@sizeOf(PhysmemStats))) == 0;

    if (!ok) {
        writeStr("  (unavailable)\n");
    } else {
        var tb: [32]u8 = undefined;
        var fb: [32]u8 = undefined;
        var ub: [32]u8 = undefined;
        const used = if (g_physmem.total_bytes >= g_physmem.free_bytes)
            g_physmem.total_bytes - g_physmem.free_bytes
        else
            0;
        writeLine(&line, "  total:    {s}\n", .{fmtBytes(g_physmem.total_bytes, &tb)});
        writeLine(&line, "  free:     {s}\n", .{fmtBytes(g_physmem.free_bytes, &fb)});
        writeLine(&line, "  used:     {s}\n", .{fmtBytes(used, &ub)});
    }

    // --- smp ---
    writeStr("\nsmp\n");
    const ncpus = sched_cpu_count();
    writeLine(&line, "  cpus:     {d}\n", .{ncpus});
    if (ncpus > 1) {
        writeStr("  smp:      enabled\n");
    } else {
        writeStr("  smp:      disabled\n");
    }

    // --- scheduler ---
    writeStr("\nscheduler\n");
    writeLine(&line, "  ticks:    {d}\n", .{sched_ticks()});
    writeLine(&line, "  ready:    {d}\n", .{sched_ready_count()});

    // --- processes ---
    writeStr("\nprocesses\n");
    writeLine(&line, "  active:   {d}\n", .{proc_count()});

    return 0;
}
