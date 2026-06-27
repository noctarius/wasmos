const std = @import("std");
const wasmos = @import("wasmos.zig");

const MAX_PROCS = 48;

extern "wasmos" fn proc_count() callconv(.c) i32;
extern "wasmos" fn proc_info_stats(
    index: i32,
    name_ptr: i32,
    name_len: i32,
    parent_ptr: i32,
    stats_ptr: i32,
) callconv(.c) i32;
extern "wasmos" fn sync_user_read(ptr: i32, len: i32) callconv(.c) i32;
extern "wasmos" fn sched_ticks() callconv(.c) i32;
extern "wasmos" fn sched_ready_count() callconv(.c) i32;
extern "wasmos" fn sched_current_pid() callconv(.c) i32;

// Must match wasmos_proc_stats_t layout in src/libc/include/wasmos/api.h.
// On wasm32 the 7 leading u32 fields (28 bytes) are followed by 4 bytes of
// implicit C ABI padding before the first u64, giving a total of 80 bytes.
const ProcStats = extern struct {
    state: u32,
    block_reason: u32,
    is_wasm: u32,
    thread_count: u32,
    live_thread_count: u32,
    current_tid: u32,
    context_id: u32,
    cpu_ticks: u64,
    vm_total_bytes: u64,
    thread_kstack_total_bytes: u64,
    heap_committed_bytes: u64,
    rss_est_bytes: u64,
    last_cpu: u32,
};

comptime {
    if (@offsetOf(ProcStats, "cpu_ticks") != 32) @compileError("ProcStats layout mismatch: cpu_ticks offset");
    if (@sizeOf(ProcStats) != 80) @compileError("ProcStats layout mismatch: total size");
}

var g_pids: [MAX_PROCS]u32 = [_]u32{0} ** MAX_PROCS;
var g_parents: [MAX_PROCS]u32 = [_]u32{0} ** MAX_PROCS;
var g_names: [MAX_PROCS][32]u8 = undefined;
var g_stats: [MAX_PROCS]ProcStats = undefined;
var g_visited: [MAX_PROCS]bool = [_]bool{false} ** MAX_PROCS;
var g_out_buf: [8192]u8 = undefined;

const OutBuf = struct {
    buf: []u8,
    len: usize = 0,
    truncated: bool = false,

    fn append(self: *OutBuf, bytes: []const u8) void {
        if (self.truncated or bytes.len == 0) return;
        const remaining = self.buf.len - self.len;
        if (bytes.len > remaining) {
            self.truncated = true;
            return;
        }
        @memcpy(self.buf[self.len .. self.len + bytes.len], bytes);
        self.len += bytes.len;
    }

    fn print(self: *OutBuf, comptime fmt: []const u8, args: anytype) void {
        if (self.truncated) return;
        const written = std.fmt.bufPrint(self.buf[self.len..], fmt, args) catch {
            self.truncated = true;
            return;
        };
        self.len += written.len;
    }

    fn flush(self: *OutBuf) void {
        if (self.len == 0) return;
        wasmos.stdlib.write(self.buf[0..self.len]) catch {};
    }
};

fn stateName(state: u32) []const u8 {
    return switch (state) {
        1 => "ready",
        2 => "run",
        3 => "blk",
        4 => "zmb",
        else => "unk",
    };
}

fn printTable(out: *OutBuf, count: usize) void {
    out.append(" pid ppid state wasm thr/live  cpu vm(bytes) kstack(bytes) heap(bytes) rss_est(bytes) cpu(ticks) name\n");
    for (0..count) |i| {
        if (g_pids[i] == 0) continue;
        const s = &g_stats[i];
        const wasm_str: []const u8 = if (s.is_wasm != 0) "true" else "false";
        out.print(
            "{d:>4} {d:>4} {s:<5} {s:<5} {d}/{d:>1}  {d:>4} {d:>10} {d:>13} {d:>11} {d:>14} {d:>10} {s}\n",
            .{
                g_pids[i],
                g_parents[i],
                stateName(s.state),
                wasm_str,
                s.thread_count,
                s.live_thread_count,
                s.last_cpu,
                s.vm_total_bytes,
                s.thread_kstack_total_bytes,
                s.heap_committed_bytes,
                s.rss_est_bytes,
                s.cpu_ticks,
                std.mem.sliceTo(&g_names[i], 0),
            },
        );
        if (out.truncated) return;
    }
}

fn findIndexByPid(count: usize, pid: u32) ?usize {
    for (0..count) |i| {
        if (g_pids[i] == pid) return i;
    }
    return null;
}

fn printTreeNode(out: *OutBuf, index: usize, count: usize, depth: u32) void {
    if (index >= count or depth > 16 or g_visited[index]) return;
    g_visited[index] = true;

    var d: u32 = 0;
    while (d < depth) : (d += 1) {
        out.append("  ");
        if (out.truncated) return;
    }

    const wasm_str: []const u8 = if (g_stats[index].is_wasm != 0) "true" else "false";
    out.print("{s} (pid {d}, wasm={s}, cpu={d})\n", .{
        std.mem.sliceTo(&g_names[index], 0),
        g_pids[index],
        wasm_str,
        g_stats[index].last_cpu,
    });
    if (out.truncated) return;

    const pid = g_pids[index];
    for (0..count) |i| {
        if (g_parents[i] == pid and i != index) {
            printTreeNode(out, i, count, depth + 1);
            if (out.truncated) return;
        }
    }
}

pub fn main() u8 {
    const args = wasmos.cliArgs();
    const show_tree = args.len > 0 and
        (std.mem.eql(u8, args[0], "tree") or std.mem.eql(u8, args[0], "all"));
    // Show the table for anything that isn't the explicit "tree" subcommand.
    // This keeps ps robust to stale/garbage CLI args (the per-context xfer
    // buffer is not zeroed on alloc, so a no-arg spawn can parse recycled
    // bytes as args). Fixing this kernel-side by zeroing the buffer triggers
    // the unresolved WARP ring3-entry fault, so we tolerate it in the consumer.
    const show_table = args.len == 0 or !std.mem.eql(u8, args[0], "tree");
    var out = OutBuf{ .buf = g_out_buf[0..] };

    const count_raw = proc_count();
    if (count_raw <= 0) {
        wasmos.stdlib.write("no processes\n") catch {};
        return 0;
    }
    const count: usize = @intCast(@min(count_raw, MAX_PROCS));

    out.print("processes: {d}\n", .{count});
    out.print("sched: ticks {d} ready {d} running {d}\n", .{
        sched_ticks(),
        sched_ready_count(),
        sched_current_pid(),
    });
    if (out.truncated) return 1;

    for (0..count) |i| {
        // g_parents[i] is a global array entry (low linear address, within user VA range).
        // Using a local variable here would place it on the Zig shadow stack at ~1MB,
        // which is outside the kernel's 32KB user VA region and would cause proc_info_stats
        // to fail the mm_user_range_permitted check.
        g_parents[i] = 0;
        const pid = proc_info_stats(
            @intCast(i),
            @intCast(@intFromPtr(&g_names[i][0])),
            @intCast(g_names[i].len),
            @intCast(@intFromPtr(&g_parents[i])),
            @intCast(@intFromPtr(&g_stats[i])),
        );
        if (pid <= 0 or
            sync_user_read(@intCast(@intFromPtr(&g_names[i][0])), @intCast(g_names[i].len)) != 0 or
            sync_user_read(@intCast(@intFromPtr(&g_parents[i])), @intCast(@sizeOf(u32))) != 0 or
            sync_user_read(@intCast(@intFromPtr(&g_stats[i])), @intCast(@sizeOf(ProcStats))) != 0)
        {
            g_pids[i] = 0;
            g_parents[i] = 0;
            g_names[i][0] = 0;
            g_visited[i] = false;
            continue;
        }
        g_pids[i] = @intCast(pid);
        g_visited[i] = false;
    }

    if (show_table) {
        printTable(&out, count);
    }

    if (show_tree) {
        out.append("tree:\n");
        for (0..count) |i| {
            if (g_pids[i] == 0) continue;
            const par_idx = findIndexByPid(count, g_parents[i]);
            if (g_parents[i] == 0 or par_idx == null or g_parents[i] == g_pids[i]) {
                printTreeNode(&out, i, count, 0);
            }
        }
        for (0..count) |i| {
            if (g_pids[i] != 0 and !g_visited[i]) {
                printTreeNode(&out, i, count, 0);
            }
        }
    }

    out.flush();

    return 0;
}
