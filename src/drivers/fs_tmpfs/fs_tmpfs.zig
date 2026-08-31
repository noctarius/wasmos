//! fs_tmpfs.zig — a read-write filesystem held in memory.
//!
//! What it is
//! ----------
//! One in-memory filesystem, mounted at the path the process that spawned it
//! named. It reports `FSMGR_BACKEND_PSEUDO` and `FS_TYPE_TMPFS`, so it is named
//! from one row of fs-manager's per-type lookup like every other filesystem.
//!
//! There is nothing root-specific about it. The mount comes from the `mount=`
//! startup argument, and a second instance mounted at `/tmp` is another process
//! with another argument and its own separate contents -- which is why every
//! identity here is DERIVED from the mount path rather than fixed: the service
//! class instance is a fingerprint of it, so two instances mounted at different
//! paths cannot claim one registry address, and two asked for the SAME path are
//! refused the claim rather than silently answering for each other.
//!
//! Contents are not persisted and are not meant to be.
//!
//! Why the VFS root wants one
//! --------------------------
//! `/` is the mount the system cannot do without, which is why the kernel's init
//! sequence spawns an instance for it and why `/` is what this driver falls back
//! to when no `mount=` is given. fs-manager routes a path to the mount that owns
//! it and does not serve a path owned by none, so with nothing mounted at `/` the
//! root is a directory that can hold nothing: `ls /` prints the mount table
//! rather than reading a directory, and a mount can only exist at the top level
//! because there is no directory anywhere to mount ONTO.
//!
//! Serving that root takes one more thing this driver cannot supply: fs-manager
//! matches a mount NAME against a path's first segment, and `/` is a mount PATH,
//! so it declines this backend until routing matches longest-prefix over mount
//! paths. See `docs/TASKS.md`.
//!
//! Storage model
//! -------------
//! A fixed node table plus a fixed pool of `BLOCK`-sized blocks, chained one
//! file at a time through `g_next`. Every static here is ZERO-initialized, which
//! is load-bearing: a Zig WASM module's initialized data must stay under the
//! layout budget `scripts/wasm_stack_check.py` enforces, so the sentinels are
//! chosen to be zero. Block 0 is never allocated, which is what lets `0` mean
//! "no block" in both `Node.first` and `g_next`; the root is node 0 and is its
//! own parent, which is what makes `..` at the root stay at the root.
//!
//! Names are case-SENSITIVE, as in WFS. FAT is the outlier there, and a mount's
//! own name is matched case-insensitively by fs-manager either way.
//!
//! One current directory per connection
//! ------------------------------------
//! A backend sees every client through fs-manager's single reply endpoint and
//! cannot tell two clients apart, so the working directory tracked here is
//! fs-manager's, not any one client's. fs-manager re-asserts it with a CHDIR
//! before every path-less request (READDIR) for exactly that reason; see
//! docs/architecture/18-filesystem-stack.md.
//!
//! Why an async service
//! --------------------
//! The manifest entry is `async_initialize`: the libsys runner owns the endpoint,
//! the event loop and the coroutine runtime, and `onMessage` is dispatched from
//! that loop. Every operation here completes in memory with no downstream call,
//! so the root task has nothing to do and parks forever on a future nothing
//! resolves — which is what makes the runner's poll park on the select set
//! instead of spinning.
const driver = @import("driver.zig");
const co = @import("coroutine.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");
const abi = @import("wasmos_constants.zig");

// --- limits ------------------------------------------------------------------

/// Longest single path component. A name is stored in the node, so this is a
/// space/limit trade rather than a protocol bound; fs-manager and the clients
/// carry paths in transfer buffers and impose no component limit of their own.
const NAME_MAX: usize = 60;
/// Files plus directories, across the whole filesystem.
const MAX_NODES: usize = 192;
/// Bytes per storage block. A file wastes at most one short block.
const BLOCK: usize = 512;
/// Blocks in the pool, of which block 0 is reserved so that 0 can mean "none".
/// The pool is the whole of this filesystem's capacity: 512 KiB.
const MAX_BLOCKS: usize = 1024;
/// Open files across all clients.
const MAX_FDS: usize = 32;
/// Connections whose working directory is tracked. fs-manager is normally the
/// only one, so this is slack rather than a budget.
const MAX_CLIENTS: usize = 8;
/// Longest path this driver will resolve in one request.
const PATH_MAX: usize = 256;
/// First descriptor handed out; 0..2 stay clear of the standard streams, as in
/// every other backend.
const FD_BASE: i32 = 3;
/// The root, which always exists and is its own parent.
const ROOT: u16 = 0;

/// The virtual class FS backends register under, and the backend KIND this one
/// reports. Declared here rather than imported because they live in
/// `src/drivers/include/wasmos_driver_abi.h`, which is C. PSEUDO is the kind of a
/// backend served from something other than a block device; it says nothing about
/// WHICH filesystem, which is what `FS_TYPE_TMPFS` answers.
const BACKEND_CLASS = "fs.backend";
const BACKEND_PSEUDO: i32 = 2;

/// This backend reports no unit. `unit` distinguishes two block-backed backends
/// on one disk; an in-memory filesystem sits on no device, so its identity is its
/// mount path and nothing else.
const BACKEND_UNIT: i32 = 0;

/// Where this instance mounts when the spawner named no `mount=`. The kernel's
/// init sequence spawns the root instance by module index, which carries no
/// arguments, so this is the mount it gets.
const DEFAULT_MOUNT = "/";

/// Longest mount path this backend can report. fs-manager holds a mount name in
/// 16 bytes (`fs_backend_t.mount_name`) and refuses a longer one, so a path past
/// this could not be registered even if it were accepted here.
const MOUNT_MAX: usize = 15;

/// Longest service name the registry stores, mirroring WASMOS_SVC_NAME_MAX.
const SVC_NAME_MAX: usize = 15;

/// Open flags, POSIX-valued, mirroring `src/libc/include/fcntl.h`. The low two
/// bits are the access mode; the rest are modifiers. Any other bit makes the
/// open bad-args, which is the contract `open()` documents.
const O_RDONLY: i32 = 0;
const O_WRONLY: i32 = 1;
const O_RDWR: i32 = 2;
const O_APPEND: i32 = 0x0008;
const O_CREAT: i32 = 0x0040;
const O_TRUNC: i32 = 0x0200;
const O_ACCESS_MASK: i32 = O_WRONLY | O_RDWR;
const O_KNOWN_MASK: i32 = O_ACCESS_MASK | O_APPEND | O_CREAT | O_TRUNC;

/// `whence` values for SEEK, mirroring `src/libc/include/unistd.h`.
const SEEK_SET: i32 = 0;
const SEEK_CUR: i32 = 1;
const SEEK_END: i32 = 2;

/// File-type bits of a STAT reply's mode word. The FS reply carries no
/// permission bits that any client reads -- libc masks everything except these
/// two -- but the mode is reported whole so a future client need not guess.
const S_IFREG: i32 = 0x8000;
const S_IFDIR: i32 = 0x4000;

// --- state -------------------------------------------------------------------

/// One file or directory. `parent` of the root is the root, so `..` there stays
/// put; `first` is 0 for a directory and for an empty file, since block 0 is
/// never allocated.
const Node = struct {
    in_use: bool = false,
    is_dir: bool = false,
    name_len: u8 = 0,
    parent: u16 = 0,
    first: u16 = 0,
    size: u32 = 0,
    name: [NAME_MAX]u8 = [_]u8{0} ** NAME_MAX,
};

/// One open file. `node` is meaningful only while `in_use`.
const Fd = struct {
    in_use: bool = false,
    node: u16 = 0,
    flags: i32 = 0,
    offset: u32 = 0,
};

/// The working directory held for one connection, keyed by the endpoint requests
/// arrive from.
const Client = struct {
    in_use: bool = false,
    source: i32 = 0,
    cwd: u16 = ROOT,
};

var g_nodes: [MAX_NODES]Node = [_]Node{.{}} ** MAX_NODES;
var g_fds: [MAX_FDS]Fd = [_]Fd{.{}} ** MAX_FDS;
var g_clients: [MAX_CLIENTS]Client = [_]Client{.{}} ** MAX_CLIENTS;

/// Next block in a file's chain, 0 terminating it.
var g_next: [MAX_BLOCKS]u16 = [_]u16{0} ** MAX_BLOCKS;
/// Whether a block is part of some file's chain. Block 0 is never allocated, so
/// its slot is never consulted.
var g_used: [MAX_BLOCKS]bool = [_]bool{false} ** MAX_BLOCKS;
/// The block pool itself: this filesystem's entire contents.
var g_store: [MAX_BLOCKS * BLOCK]u8 = [_]u8{0} ** (MAX_BLOCKS * BLOCK);

/// Path scratch, static rather than stack: a Zig WASM module here runs on an
/// 8 KiB shadow stack, so a per-request buffer of this size belongs in .bss.
var g_path: [PATH_MAX]u8 = [_]u8{0} ** PATH_MAX;
var g_path2: [PATH_MAX]u8 = [_]u8{0} ** PATH_MAX;
/// One block's worth of staging for a read or a write, for the same reason.
var g_stage: [BLOCK]u8 = [_]u8{0} ** BLOCK;
/// One directory entry plus its trailing '/' and newline, for READDIR.
var g_line: [NAME_MAX + 2]u8 = [_]u8{0} ** (NAME_MAX + 2);

var g_proc_endpoint: i32 = -1;

/// This instance's mount path, and the concrete service name derived from it.
/// Both are settled in `prepare` and never change: a mount is decided by whoever
/// spawned this process, not by anything it can observe later.
var g_mount: [MOUNT_MAX]u8 = [_]u8{0} ** MOUNT_MAX;
var g_mount_len: usize = 0;
var g_service_name: [SVC_NAME_MAX]u8 = [_]u8{0} ** SVC_NAME_MAX;
var g_service_name_len: usize = 0;

fn mountPath() []const u8 {
    return g_mount[0..g_mount_len];
}

/// The root task's park. Nothing resolves this promise: every request is served
/// from the message handler, so the root has no work and the loop's poll is what
/// this process sleeps in.
var g_park_future: co.Future = .{};
var g_park_promise: co.Promise = .{};

// --- async service contract --------------------------------------------------

/// Definition of one async WASM guest, mirroring `wasmos_sys_wasm_async_config_t`.
/// The runtime, root task, event loop and reply endpoint are scratch the libsys
/// runner fills in; only `resume`, `prepare` and `user` are inputs.
const AsyncServiceConfig = extern struct {
    runtime: co.Runtime = .{},
    root: co.Coroutine = .{},
    event_loop: co.EventLoop = .{},
    reply_endpoint: i32 = 0,
    @"resume": ?co.TaskResume = null,
    prepare: ?*const fn (?*anyopaque, i32, i32, i32, i32) callconv(.c) void = null,
    user: ?*anyopaque = null,
};

export var wasmos_async_service: AsyncServiceConfig = .{
    .@"resume" = rootTask,
    .prepare = prepare,
};

fn loop() *co.EventLoop {
    return &wasmos_async_service.event_loop;
}

/// The endpoint the runner created. This filesystem is served on it and it is
/// what the `fs.backend` class registration publishes.
fn endpoint() i32 {
    return wasmos_async_service.reply_endpoint;
}

// --- names -------------------------------------------------------------------

/// A node's name as a slice of its own storage.
fn nameOf(n: u16) []const u8 {
    const node = &g_nodes[n];
    return node.name[0..node.name_len];
}

fn nameEqual(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| {
        if (x != y) return false;
    }
    return true;
}

/// The entry named `name` in directory `dir`.
fn lookup(dir: u16, name: []const u8) ?u16 {
    var i: u16 = 0;
    while (i < MAX_NODES) : (i += 1) {
        const node = &g_nodes[i];
        if (!node.in_use or node.parent != dir or i == ROOT) continue;
        if (nameEqual(nameOf(i), name)) return i;
    }
    return null;
}

/// Any entry of `dir`, or null when it holds none.
fn lookupAny(dir: u16) ?u16 {
    var i: u16 = 0;
    while (i < MAX_NODES) : (i += 1) {
        if (g_nodes[i].in_use and i != ROOT and g_nodes[i].parent == dir) return i;
    }
    return null;
}

/// Whether `maybe_ancestor` is `n` or lies above it. Used to refuse renaming a
/// directory into its own subtree, which would detach it from the root.
fn isAncestor(maybe_ancestor: u16, n: u16) bool {
    var cur = n;
    while (true) {
        if (cur == maybe_ancestor) return true;
        if (cur == ROOT) return false;
        cur = g_nodes[cur].parent;
    }
}

// --- blocks ------------------------------------------------------------------

/// Claim a free block, zeroed. Block 0 is skipped so that 0 can mean "none".
fn blockAlloc() ?u16 {
    var i: usize = 1;
    while (i < MAX_BLOCKS) : (i += 1) {
        if (g_used[i]) continue;
        g_used[i] = true;
        g_next[i] = 0;
        const base = i * BLOCK;
        @memset(g_store[base .. base + BLOCK], 0);
        return @intCast(i);
    }
    return null;
}

/// Index of the `want`-th block of `n`'s chain, or null when the chain is
/// shorter than that.
fn blockAt(n: u16, want: usize) ?u16 {
    var block = g_nodes[n].first;
    var i: usize = 0;
    while (block != 0) {
        if (i == want) return block;
        i += 1;
        block = g_next[block];
    }
    return null;
}

/// The block holding byte `off` of `n`, appending blocks as far as needed.
/// Returns null when the pool is full, having kept the chain consistent.
fn blockForWrite(n: u16, off: u32) ?u16 {
    const want: usize = @as(usize, off) / BLOCK;
    if (blockAt(n, want)) |block| return block;

    // Append from the chain's tail rather than from its head, so a sparse write
    // past the end fills the gap with zeroed blocks instead of leaving a hole
    // this layout cannot express.
    var tail: u16 = 0;
    var have: usize = 0;
    var block = g_nodes[n].first;
    while (block != 0) {
        tail = block;
        have += 1;
        block = g_next[block];
    }
    while (have <= want) {
        const fresh = blockAlloc() orelse return null;
        if (tail == 0) {
            g_nodes[n].first = fresh;
        } else {
            g_next[tail] = fresh;
        }
        tail = fresh;
        have += 1;
    }
    return tail;
}

/// Release every block of `n` from block index `keep` onward.
fn truncateBlocks(n: u16, keep: usize) void {
    var prev: u16 = 0;
    var block = g_nodes[n].first;
    var i: usize = 0;
    while (block != 0 and i < keep) {
        prev = block;
        block = g_next[block];
        i += 1;
    }
    if (prev == 0) {
        g_nodes[n].first = 0;
    } else {
        g_next[prev] = 0;
    }
    while (block != 0) {
        const following = g_next[block];
        g_used[block] = false;
        g_next[block] = 0;
        block = following;
    }
}

/// Copy out of `n` at `off` into `dst`, stopping at the file's size. Returns how
/// many bytes were copied.
fn readAt(n: u16, off: u32, dst: []u8) u32 {
    const size = g_nodes[n].size;
    if (off >= size) return 0;
    var want: u32 = @intCast(dst.len);
    if (want > size - off) want = size - off;

    var done: u32 = 0;
    while (done < want) {
        const at = off + done;
        const block = blockAt(n, @as(usize, at) / BLOCK) orelse break;
        const within: usize = @as(usize, at) % BLOCK;
        var chunk: usize = BLOCK - within;
        if (chunk > want - done) chunk = want - done;
        const base = @as(usize, block) * BLOCK + within;
        @memcpy(dst[done .. done + chunk], g_store[base .. base + chunk]);
        done += @intCast(chunk);
    }
    return done;
}

/// Copy `src` into `n` at `off`, growing the file as needed. Returns how many
/// bytes were stored, which is SHORT when the pool ran out -- the same shape a
/// full disk gives, so a caller reports what it wrote rather than failing whole.
fn writeAt(n: u16, off: u32, src: []const u8) u32 {
    var done: u32 = 0;
    while (done < src.len) {
        const at = off + done;
        const block = blockForWrite(n, at) orelse break;
        const within: usize = @as(usize, at) % BLOCK;
        var chunk: usize = BLOCK - within;
        if (chunk > src.len - done) chunk = src.len - done;
        const base = @as(usize, block) * BLOCK + within;
        @memcpy(g_store[base .. base + chunk], src[done .. done + chunk]);
        done += @intCast(chunk);
        if (at + chunk > g_nodes[n].size) g_nodes[n].size = at + @as(u32, @intCast(chunk));
    }
    return done;
}

// --- nodes -------------------------------------------------------------------

/// Create an entry named `name` in `parent`. The caller has already established
/// that no such entry exists.
fn nodeAlloc(parent: u16, name: []const u8, is_dir: bool) ?u16 {
    if (name.len == 0 or name.len > NAME_MAX) return null;
    var i: u16 = 1; // node 0 is the root and is never handed out
    while (i < MAX_NODES) : (i += 1) {
        if (g_nodes[i].in_use) continue;
        g_nodes[i] = .{
            .in_use = true,
            .is_dir = is_dir,
            .name_len = @intCast(name.len),
            .parent = parent,
        };
        @memcpy(g_nodes[i].name[0..name.len], name);
        return i;
    }
    return null;
}

/// Release `n` and everything it stored. The caller has established that it is
/// empty (a directory) and unopened.
fn nodeFree(n: u16) void {
    truncateBlocks(n, 0);
    g_nodes[n] = .{};
}

/// Whether any descriptor names `n`. A node cannot be removed or renamed over
/// while one does: a descriptor holds the node index, so freeing it under an open
/// file would leave that descriptor addressing a reused node.
fn nodeIsOpen(n: u16) bool {
    for (&g_fds) |*fd| {
        if (fd.in_use and fd.node == n) return true;
    }
    return false;
}

// --- paths -------------------------------------------------------------------

/// Resolve `path` from `cwd` to the node it names.
///
/// An absolute path restarts at the root, `.` keeps the current node, `..` moves
/// to the parent and cannot leave the root, and repeated slashes collapse.
/// Returns null when a component does not exist or when a non-final component is
/// not a directory.
fn resolve(cwd: u16, path: []const u8) ?u16 {
    var cur: u16 = if (path.len > 0 and path[0] == '/') ROOT else cwd;
    var i: usize = 0;
    while (i < path.len) {
        if (path[i] == '/') {
            i += 1;
            continue;
        }
        const start = i;
        while (i < path.len and path[i] != '/') i += 1;
        const seg = path[start..i];
        if (seg.len == 1 and seg[0] == '.') continue;
        if (seg.len == 2 and seg[0] == '.' and seg[1] == '.') {
            cur = g_nodes[cur].parent;
            continue;
        }
        if (!g_nodes[cur].is_dir) return null;
        cur = lookup(cur, seg) orelse return null;
    }
    return cur;
}

/// A path's last component and the directory that holds it.
const Split = struct { parent: u16, name: []const u8 };

/// Split `path` into the directory holding its last component and that
/// component.
///
/// What every create and remove needs: the parent must exist and be a directory,
/// while the component itself may or may not. Returns null when the parent does
/// not resolve, when the path names no component at all (the root), when the last
/// component is `.` or `..` -- neither of which names a thing to create or remove
/// -- or when the name does not fit a node.
fn splitPath(cwd: u16, path: []const u8) ?Split {
    var last_start: usize = 0;
    var last_end: usize = 0;
    var i: usize = 0;
    while (i < path.len) {
        if (path[i] == '/') {
            i += 1;
            continue;
        }
        last_start = i;
        while (i < path.len and path[i] != '/') i += 1;
        last_end = i;
    }
    if (last_end == last_start) return null;
    const name = path[last_start..last_end];
    if (name.len > NAME_MAX) return null;
    if (nameEqual(name, ".") or nameEqual(name, "..")) return null;

    const parent = resolve(cwd, path[0..last_start]) orelse return null;
    if (!g_nodes[parent].is_dir) return null;
    return .{ .parent = parent, .name = name };
}

// --- request plumbing --------------------------------------------------------

/// A path taken out of a client's transfer buffer, or the packed reason it could
/// not be.
const Path = union(enum) { ok: []const u8, err: i32 };

/// Copy a path out of the buffer the client lent this backend.
///
/// An EMPTY path is bad-args rather than too-long: a caller told "too long"
/// would shorten a name it never sent. `out` is one of the static scratch
/// buffers, so the returned slice is valid until the next request reuses it.
fn takePath(buffer_id: i32, len: i32, offset: i32, out: []u8) Path {
    if (buffer_id <= 0 or len <= 0) return .{ .err = status.WASMOS_ERR_FS_BAD_ARGS };
    if (offset < 0) return .{ .err = status.WASMOS_ERR_FS_BAD_ARGS };
    const want: usize = @intCast(len);
    if (want >= out.len) return .{ .err = status.WASMOS_ERR_FS_PATH_TOO_LONG };
    if (want + @as(usize, @intCast(offset)) > driver.xferBufferSize()) {
        return .{ .err = status.WASMOS_ERR_FS_PATH_TOO_LONG };
    }
    if (!driver.bufferReadBytes(buffer_id, out[0..want], offset)) {
        return .{ .err = status.WASMOS_ERR_FS_BUFFER };
    }
    return .{ .ok = out[0..want] };
}

/// The working directory held for the endpoint `source`, claiming a slot on first
/// sight. Null when every slot is taken.
fn clientCwd(source: i32) ?*u16 {
    var free: ?*Client = null;
    for (&g_clients) |*client| {
        if (client.in_use and client.source == source) return &client.cwd;
        if (!client.in_use and free == null) free = client;
    }
    const slot = free orelse return null;
    slot.* = .{ .in_use = true, .source = source, .cwd = ROOT };
    return &slot.cwd;
}

/// Answer a request. A negative `a0` is a packed reason and makes this an error
/// reply, which is why every status this driver produces is either a datum or a
/// code from `abi/errors.yaml`.
fn reply(dest: i32, request_id: i32, a0: i32, a1: i32) void {
    const kind: i32 = if (a0 >= 0) op.FS_IPC_RESP else op.FS_IPC_ERROR;
    _ = driver.send(dest, endpoint(), kind, request_id, a0, a1, 0, 0);
}

fn fail(dest: i32, request_id: i32, code: i32) void {
    _ = driver.send(dest, endpoint(), op.FS_IPC_ERROR, request_id, code, 0, 0, 0);
}

fn fdLookup(fd: i32) ?*Fd {
    if (fd < FD_BASE or fd >= FD_BASE + @as(i32, MAX_FDS)) return null;
    const slot = &g_fds[@intCast(fd - FD_BASE)];
    return if (slot.in_use) slot else null;
}

// --- operations --------------------------------------------------------------

/// Report this backend's identity into the buffer fs-manager supplied.
///
/// fs-manager is the CLIENT of this exchange and owns any buffer it carries
/// (docs/architecture/12-dma-transfers.md), so this writes into a grant and owns
/// nothing. `kind` places this backend outside the block-backed set and `fs_type`
/// names the filesystem; a reply whose name length is 0 is malformed and the
/// backend is not registered.
fn handleBackendInfo(msg: *const co.IpcMessage) void {
    var name_len: i32 = 0;
    if (msg.arg0 > 0 and g_mount_len > 0 and driver.bufferWrite(msg.arg0, mountPath(), 0)) {
        name_len = @intCast(g_mount_len);
    }
    _ = driver.send(
        msg.source,
        endpoint(),
        op.FSMGR_IPC_BACKEND_INFO_RESP,
        msg.request_id,
        BACKEND_PSEUDO,
        abi.FS_TYPE_TMPFS,
        name_len,
        BACKEND_UNIT,
    );
}

/// OPEN: arg0 = path length, arg1 = flags, arg2 = the client's buffer holding the
/// path.
fn handleOpen(cwd: u16, msg: *const co.IpcMessage) void {
    const path = switch (takePath(msg.arg2, msg.arg0, 0, &g_path)) {
        .err => |code| return fail(msg.source, msg.request_id, code),
        .ok => |p| p,
    };
    const flags = msg.arg1;
    if ((flags & ~O_KNOWN_MASK) != 0 or (flags & O_ACCESS_MASK) == O_ACCESS_MASK) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS);
    }
    const writable = (flags & O_ACCESS_MASK) != 0;
    if (!writable and (flags & (O_APPEND | O_TRUNC)) != 0) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS);
    }

    var node = resolve(cwd, path);
    if (node == null and (flags & O_CREAT) != 0) {
        const split = splitPath(cwd, path) orelse
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
        node = nodeAlloc(split.parent, split.name, false) orelse
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NO_SPACE);
    }
    const target = node orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
    if (g_nodes[target].is_dir) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_IS_DIR);
    }

    // O_TRUNC applies BEFORE the descriptor is handed out, so the offset an
    // O_APPEND open latches is the truncated size rather than the old one.
    if ((flags & O_TRUNC) != 0 and g_nodes[target].size != 0) {
        truncateBlocks(target, 0);
        g_nodes[target].size = 0;
    }

    for (&g_fds, 0..) |*fd, i| {
        if (fd.in_use) continue;
        fd.* = .{
            .in_use = true,
            .node = target,
            .flags = flags,
            .offset = if ((flags & O_APPEND) != 0) g_nodes[target].size else 0,
        };
        return reply(msg.source, msg.request_id, FD_BASE + @as(i32, @intCast(i)), 0);
    }
    fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NO_FD);
}

/// STAT: arg0 = path length, arg2 = the client's buffer. Replies with the size in
/// arg0 and the mode in arg1.
fn handleStat(cwd: u16, msg: *const co.IpcMessage) void {
    const path = switch (takePath(msg.arg2, msg.arg0, 0, &g_path)) {
        .err => |code| return fail(msg.source, msg.request_id, code),
        .ok => |p| p,
    };
    const node = resolve(cwd, path) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
    const mode: i32 = if (g_nodes[node].is_dir) S_IFDIR | 0o755 else S_IFREG | 0o644;
    reply(msg.source, msg.request_id, @intCast(g_nodes[node].size), mode);
}

/// READ: arg0 = fd, arg1 = byte count, arg2 = the client's buffer to fill.
fn handleRead(msg: *const co.IpcMessage) void {
    const fd = fdLookup(msg.arg0) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_FD);
    if (msg.arg1 < 0) return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS);
    if ((fd.flags & O_ACCESS_MASK) == O_WRONLY) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_ACCESS);
    }

    var want: usize = @intCast(msg.arg1);
    const room = driver.xferBufferSize();
    if (want > room) want = room;

    var done: usize = 0;
    while (done < want) {
        var chunk: usize = want - done;
        if (chunk > BLOCK) chunk = BLOCK;
        const got: usize = readAt(fd.node, fd.offset + @as(u32, @intCast(done)), g_stage[0..chunk]);
        if (got == 0) break;
        if (!driver.bufferWrite(msg.arg2, g_stage[0..got], @intCast(done))) {
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BUFFER);
        }
        done += got;
        if (got < chunk) break;
    }
    fd.offset += @intCast(done);
    reply(msg.source, msg.request_id, @intCast(done), 0);
}

/// WRITE: arg0 = fd, arg1 = byte count, arg2 = the client's buffer to drain.
fn handleWrite(msg: *const co.IpcMessage) void {
    const fd = fdLookup(msg.arg0) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_FD);
    if (msg.arg1 < 0) return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS);
    // An access-mode violation on the descriptor, whatever the filesystem
    // permits: this one was opened for reading.
    if ((fd.flags & O_ACCESS_MASK) == 0) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_ACCESS);
    }

    var want: usize = @intCast(msg.arg1);
    const room = driver.xferBufferSize();
    if (want > room) want = room;

    var done: usize = 0;
    while (done < want) {
        var chunk: usize = want - done;
        if (chunk > BLOCK) chunk = BLOCK;
        if (!driver.bufferReadBytes(msg.arg2, g_stage[0..chunk], @intCast(done))) {
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BUFFER);
        }
        const stored: usize = writeAt(fd.node, fd.offset + @as(u32, @intCast(done)), g_stage[0..chunk]);
        done += stored;
        if (stored < chunk) break; // the pool is full: report a short write
    }
    if (done == 0 and want != 0) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NO_SPACE);
    }
    fd.offset += @intCast(done);
    reply(msg.source, msg.request_id, @intCast(done), 0);
}

/// SEEK: arg0 = fd, arg1 = delta, arg2 = whence. An offset outside [0, size] is
/// refused rather than clamped, so SEEK_END with a positive delta fails instead
/// of extending the file.
fn handleSeek(msg: *const co.IpcMessage) void {
    const fd = fdLookup(msg.arg0) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_FD);
    const size: i64 = g_nodes[fd.node].size;
    const base: i64 = switch (msg.arg2) {
        SEEK_SET => 0,
        SEEK_CUR => @intCast(fd.offset),
        SEEK_END => size,
        else => return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS),
    };
    const target = base + @as(i64, msg.arg1);
    if (target < 0 or target > size) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_RANGE);
    }
    fd.offset = @intCast(target);
    reply(msg.source, msg.request_id, @intCast(target), 0);
}

/// CLOSE: arg0 = fd.
fn handleClose(msg: *const co.IpcMessage) void {
    const fd = fdLookup(msg.arg0) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_FD);
    fd.* = .{};
    reply(msg.source, msg.request_id, 0, 0);
}

/// CHDIR: arg0 = path length, arg2 = the client's buffer. A zero length names the
/// filesystem's own root, which is how fs-manager asks for it.
fn handleChdir(cwd: *u16, msg: *const co.IpcMessage) void {
    if (msg.arg0 == 0) {
        cwd.* = ROOT;
        return reply(msg.source, msg.request_id, 0, 0);
    }
    const path = switch (takePath(msg.arg2, msg.arg0, 0, &g_path)) {
        .err => |code| return fail(msg.source, msg.request_id, code),
        .ok => |p| p,
    };
    const node = resolve(cwd.*, path) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
    if (!g_nodes[node].is_dir) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_DIR);
    }
    cwd.* = node;
    reply(msg.source, msg.request_id, 0, 0);
}

/// Stream `bytes` to a READDIR client, four per FS_IPC_STREAM frame -- the shape
/// the client reassembles and every other backend emits. `driver.send` treats a
/// full receiver queue as backpressure and retries, which matters here because
/// the client is blocked on exactly these frames.
fn stream(dest: i32, request_id: i32, bytes: []const u8) bool {
    var pos: usize = 0;
    while (pos < bytes.len) {
        var a = [4]i32{ 0, 0, 0, 0 };
        var i: usize = 0;
        while (i < 4 and pos < bytes.len) : (i += 1) {
            a[i] = bytes[pos];
            pos += 1;
        }
        if (driver.send(dest, endpoint(), op.FS_IPC_STREAM, request_id, a[0], a[1], a[2], a[3]) != 0) {
            return false;
        }
    }
    return true;
}

/// READDIR: the entries of the directory this connection currently stands in.
///
/// Directories carry a trailing '/', which is what the CLI renders. `.` and `..`
/// are not listed, matching every other backend: they resolve whether or not they
/// appear.
fn handleReaddir(cwd: u16, msg: *const co.IpcMessage) void {
    if (!g_nodes[cwd].is_dir) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_DIR);
    }
    var i: u16 = 0;
    while (i < MAX_NODES) : (i += 1) {
        if (!g_nodes[i].in_use or i == ROOT or g_nodes[i].parent != cwd) continue;
        const name = nameOf(i);
        var n: usize = 0;
        @memcpy(g_line[0..name.len], name);
        n += name.len;
        if (g_nodes[i].is_dir) {
            g_line[n] = '/';
            n += 1;
        }
        g_line[n] = '\n';
        n += 1;
        if (!stream(msg.source, msg.request_id, g_line[0..n])) {
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_REPLY_SEND);
        }
    }
    reply(msg.source, msg.request_id, 0, 0);
}

/// MKDIR, UNLINK and RMDIR: arg0 = path length, arg2 = the client's buffer.
///
/// A node with a descriptor open on it is refused rather than freed: a descriptor
/// holds the node index, so removing it underneath one would leave that
/// descriptor addressing a slot the next create reuses.
fn handleNamespace(cwd: u16, msg: *const co.IpcMessage) void {
    const path = switch (takePath(msg.arg2, msg.arg0, 0, &g_path)) {
        .err => |code| return fail(msg.source, msg.request_id, code),
        .ok => |p| p,
    };
    const split = splitPath(cwd, path) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
    const existing = lookup(split.parent, split.name);

    switch (msg.type) {
        op.FS_IPC_MKDIR_REQ => {
            if (existing != null) {
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_EXISTS);
            }
            _ = nodeAlloc(split.parent, split.name, true) orelse
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NO_SPACE);
        },
        op.FS_IPC_UNLINK_REQ => {
            const node = existing orelse
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
            if (g_nodes[node].is_dir) {
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_IS_DIR);
            }
            if (nodeIsOpen(node)) {
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_OPEN);
            }
            nodeFree(node);
        },
        op.FS_IPC_RMDIR_REQ => {
            const node = existing orelse
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
            if (!g_nodes[node].is_dir) {
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_DIR);
            }
            if (lookupAny(node) != null) {
                return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_EMPTY);
            }
            // A directory this connection stands in would leave the cwd pointing
            // at a freed node, so it is refused like an open file.
            for (&g_clients) |*client| {
                if (client.in_use and client.cwd == node) {
                    return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_OPEN);
                }
            }
            nodeFree(node);
        },
        else => return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_UNSUPPORTED),
    }
    reply(msg.source, msg.request_id, 0, 0);
}

/// RENAME: both paths travel in one buffer, the source at offset 0 and the
/// destination at arg0 + 1 -- the layout fs-manager's routing rewrites them
/// under. arg0 and arg1 are their lengths.
///
/// An existing destination FILE is replaced and its data released, as POSIX
/// requires; an existing destination DIRECTORY is refused, because freeing what
/// it holds would be a recursive delete rather than a rename. An open source or
/// destination is refused, since a descriptor names the node it was opened on.
fn handleRename(cwd: u16, msg: *const co.IpcMessage) void {
    const from = switch (takePath(msg.arg2, msg.arg0, 0, &g_path)) {
        .err => |code| return fail(msg.source, msg.request_id, code),
        .ok => |p| p,
    };
    const to = switch (takePath(msg.arg2, msg.arg1, msg.arg0 + 1, &g_path2)) {
        .err => |code| return fail(msg.source, msg.request_id, code),
        .ok => |p| p,
    };
    const src = resolve(cwd, from) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
    if (src == ROOT) return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS);
    const dst = splitPath(cwd, to) orelse
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NOT_FOUND);
    if (nodeIsOpen(src)) return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BUSY);
    // A directory cannot be moved beneath itself: the subtree would leave the
    // root and be unreachable from every path.
    if (g_nodes[src].is_dir and isAncestor(src, dst.parent)) {
        return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BAD_ARGS);
    }

    if (lookup(dst.parent, dst.name)) |victim| {
        if (victim == src) return reply(msg.source, msg.request_id, 0, 0);
        if (g_nodes[victim].is_dir) {
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_IS_DIR);
        }
        if (nodeIsOpen(victim)) {
            return fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_BUSY);
        }
        nodeFree(victim);
    }
    g_nodes[src].parent = dst.parent;
    g_nodes[src].name_len = @intCast(dst.name.len);
    @memset(&g_nodes[src].name, 0);
    @memcpy(g_nodes[src].name[0..dst.name.len], dst.name);
    reply(msg.source, msg.request_id, 0, 0);
}

// --- dispatch ----------------------------------------------------------------

fn onMessage(user: ?*anyopaque, msg: *const co.IpcMessage) callconv(.c) void {
    _ = user;
    if (msg.type == op.FSMGR_IPC_BACKEND_INFO_REQ) {
        handleBackendInfo(msg);
        return;
    }
    const cwd = clientCwd(msg.source) orelse {
        fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_NO_CLIENT_SLOT);
        return;
    };
    switch (msg.type) {
        op.FS_IPC_READY_REQ => reply(msg.source, msg.request_id, 0, 0),
        op.FS_IPC_OPEN_REQ => handleOpen(cwd.*, msg),
        op.FS_IPC_STAT_REQ => handleStat(cwd.*, msg),
        op.FS_IPC_READ_REQ => handleRead(msg),
        op.FS_IPC_WRITE_REQ => handleWrite(msg),
        op.FS_IPC_SEEK_REQ => handleSeek(msg),
        op.FS_IPC_CLOSE_REQ => handleClose(msg),
        op.FS_IPC_CHDIR_REQ => handleChdir(cwd, msg),
        op.FS_IPC_READDIR_REQ => handleReaddir(cwd.*, msg),
        op.FS_IPC_MKDIR_REQ, op.FS_IPC_UNLINK_REQ, op.FS_IPC_RMDIR_REQ => handleNamespace(cwd.*, msg),
        op.FS_IPC_RENAME_REQ => handleRename(cwd.*, msg),
        // Everything else, including the retired READ_APP: this backend says so
        // rather than staying silent, because a client with no reply waits
        // forever.
        else => fail(msg.source, msg.request_id, status.WASMOS_ERR_FS_UNSUPPORTED),
    }
}

// --- bring-up ----------------------------------------------------------------

/// Settle this instance's mount path from the `mount=` startup argument, falling
/// back to the root when the spawner named none.
///
/// The mount is a decision made by whoever spawned this process, not something
/// this one can observe, so it is read once and never revisited. The root
/// instance the kernel's init spawns by module index gets no arguments at all,
/// which is what the fallback is for. A path that is not absolute is refused
/// rather than repaired: a relative mount would name a different directory
/// depending on who asked.
///
/// Returns false when the argument is present but unusable, which leaves this
/// process running but unregistered rather than mounted somewhere it was not
/// asked to be.
///
/// TODO: a SUBSYSTEM=="boot" device-manager rule parses only SUBSYSTEM and RUN
/// (parse_always_spawn_rule_line), so it cannot carry ENV{MOUNT} and a second
/// tmpfs instance cannot yet be spawned from the rule files. Until it can, a
/// non-root instance needs a spawner that passes startup arguments itself.
fn resolveMount() bool {
    const requested = driver.findArg(driver.startupArgs(), "mount=") orelse DEFAULT_MOUNT;
    if (requested.len == 0 or requested[0] != '/' or requested.len > MOUNT_MAX) {
        var line = driver.Line{};
        _ = line.str("[fs-tmpfs] unusable mount=").str(requested).str("; not registering");
        line.end();
        return false;
    }
    @memcpy(g_mount[0..requested.len], requested);
    g_mount_len = requested.len;

    // The concrete service name is diagnostic -- fs-manager finds backends by
    // CLASS, and nothing looks a filesystem backend up by name -- but it must
    // still be non-empty and it reads better naming the mount than the driver.
    // A path too long to append is truncated, which costs a duplicate name and
    // nothing else; the class INSTANCE is the identity that has to be unique, and
    // it is a fingerprint of the whole path.
    const prefix = "tmpfs";
    @memcpy(g_service_name[0..prefix.len], prefix);
    g_service_name_len = prefix.len;
    var i: usize = 0;
    while (i < requested.len and g_service_name_len < SVC_NAME_MAX) : (i += 1) {
        g_service_name[g_service_name_len] = requested[i];
        g_service_name_len += 1;
    }
    return true;
}

fn prepare(user: ?*anyopaque, arg0: i32, arg1: i32, arg2: i32, arg3: i32) callconv(.c) void {
    _ = user;
    _ = arg0;
    _ = arg1;
    _ = arg2;
    _ = arg3;

    loop().default_on_message = @ptrCast(&onMessage);
    g_proc_endpoint = driver.procEndpoint();

    // This filesystem's own root exists before anything can ask for it, and is
    // the one node no request creates or removes.
    g_nodes[ROOT] = .{ .in_use = true, .is_dir = true, .parent = ROOT };

    if (!resolveMount()) return;

    // Register the concrete name plus the `fs.backend` class. fs-manager
    // discovers backends through that class and PULLS the mount info, so there is
    // no push registration here. Claiming a class needs the svc.class capability
    // (see linker.metadata).
    //
    // The instance is a fingerprint of the mount path rather than a packed
    // (kind, unit) pair: a tmpfs has no unit, and the path is the only thing that
    // tells two instances apart. The registry refuses a second claim on a live
    // instance, so two instances asked to mount at one path cannot both serve it.
    // The refusal does not surface here as a failure, though: the process manager
    // answers a refused claim with no reply at all, so this call blocks rather
    // than returning null (FIXME in process_manager_services.c).
    if (driver.registerService(
        g_proc_endpoint,
        endpoint(),
        g_service_name[0..g_service_name_len],
        BACKEND_CLASS,
        driver.fingerprint(mountPath()),
        1,
    ) == null) {
        var line = driver.Line{};
        _ = line.str("[fs-tmpfs] fs.backend register failed for mount ").str(mountPath());
        line.end();
        return;
    }

    var line = driver.Line{};
    _ = line.str("[fs-tmpfs] ready mount=").str(mountPath());
    _ = line.str(" nodes=").dec(MAX_NODES);
    _ = line.str(" capacity=").dec((MAX_BLOCKS - 1) * BLOCK).str("B");
    line.end();

    // Ready is signalled here rather than deferred until fs-manager's first pull,
    // as fs-init defers it: nothing in the system waits on this driver's readiness
    // to decide whether a mount exists, and a deferral would leave the process
    // unready forever if the class pull never came.
    driver.notifyReady(g_proc_endpoint);
}

/// The root task parks and stays parked.
///
/// Every request is served from `onMessage`, which runs on the event loop, so
/// there is no work here at all. Parking on a future nothing resolves is what
/// keeps the runner in `wasmos_sys_event_loop_poll`, which blocks on the loop's
/// select set -- an idle tmpfs therefore sleeps rather than spins.
fn rootTask(user: ?*anyopaque, out_value: *usize) callconv(.c) i32 {
    _ = user;
    _ = out_value;
    g_park_future.init(&g_park_promise);
    switch (g_park_future.awaitValue()) {
        .pending => return co.TaskResult.yielded,
        // The runtime refused the park, which it does only for a future owned by
        // another runtime. Yield rather than complete: a completed root task ends
        // the runner's loop and takes the filesystem down with it.
        else => return co.TaskResult.yielded,
    }
}
