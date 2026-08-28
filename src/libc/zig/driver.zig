//! Driver-side runtime for Zig WASM device drivers.
//!
//! `wasmos.zig` is the APPLICATION shim: it owns `wasmos_main`, calls the root
//! module's `main`, and offers a filesystem and a console. A driver is a
//! different kind of module. It is entered at `initialize` rather than
//! `wasmos_main`, it never returns, and what it needs is the surface an
//! application deliberately does not get -- granted I/O windows, pinned DMA
//! regions, interrupt routing, and the service registry. That surface is here,
//! so a driver can import this module alone and stay out of the application
//! entry contract (importing `wasmos.zig` would require the module to export a
//! `main` it has no use for).
//!
//! It offers no receive loop, no select set and no drain: a driver is an async
//! service (`async_initialize`, `wasmos_async_service`), and the libsys runner
//! owns its endpoint, event loop and pump. Serving work belongs in a coroutine
//! parked on a future, never in a loop written here -- see
//! `src/drivers/virtio_blk/virtio_blk.zig` for the shape, and
//! docs/architecture/32-coroutines-futures-promises.md for why.
//!
//! Everything below is a thin, typed face over the generated ABI in
//! `abi/generated/zig/`; the wire protocols it implements -- the service
//! register descriptor and the ready handshake -- are the same ones
//! `wasmos/ipc.h` and `wasmos/libsys.h` implement for C, and the two must stay
//! in step (see `src/drivers/include/wasmos_driver_abi.h`, which is the
//! authority for the descriptor layout).

const abi = @import("wasmos_imports.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");

/// The one kernel IPC status the send helper acts on rather than propagates: a
/// destination queue that was full and is worth retrying. Mirrors the kernel's
/// IPC_ERR_FULL.
const IPC_ERR_FULL: i32 = -3;
/// Send attempts before a queue-full failure is reported, one scheduler yield
/// apart. Large because the peer only has to be scheduled once to drain its
/// queue: it bounds a livelock, not a latency.
const IPC_SEND_RETRY_LIMIT: u32 = 4096;
/// Cache policy for `regionAlloc`. Write-back is coherent and is what a
/// virtqueue ring on x86 wants; write-combining is for scanout memory.
pub const REGION_CACHE_WB: i32 = 0;
pub const REGION_CACHE_WC: i32 = 1;

/// Direction of a DMA mapping, named from the DEVICE's point of view, matching
/// the `WASMOS_DMA_DIR_*` BIT FLAGS in `wasmos_driver_abi.h`.
pub const DMA_DIR_TO_DEVICE: i32 = 1 << 0;
pub const DMA_DIR_FROM_DEVICE: i32 = 1 << 1;
pub const DMA_DIR_BIDIR: i32 = DMA_DIR_TO_DEVICE | DMA_DIR_FROM_DEVICE;

/// Operation of a DMA sync. These are ENUMERATED values (1/2/3) and not the bit
/// flags above, despite reading the same for the two single directions --
/// `WASMOS_DMA_SYNC_*` in `wasmos_driver_abi.h` is the authority.
pub const DMA_SYNC_TO_DEVICE: i32 = 1;
pub const DMA_SYNC_FROM_DEVICE: i32 = 2;
pub const DMA_SYNC_BIDIR: i32 = 3;

/// Service-name and class-name capacities, both INCLUDING the terminating NUL.
/// These fix the byte length of the register descriptor below, so they must
/// stay equal to WASMOS_SVC_NAME_MAX / WASMOS_SVC_CLASS_MAX in
/// `src/drivers/include/wasmos_driver_abi.h`.
const SVC_NAME_MAX: usize = 36;
const SVC_CLASS_MAX: usize = 16;
const SVC_REGISTER_DESC_VERSION: u32 = 2;

/// Register descriptor written to the transfer buffer for
/// SVC_IPC_REGISTER_DESC_REQ. Field order, types and therefore offsets mirror
/// `svc_register_desc_t`; the process manager discriminates v1 from v2 by the
/// byte length it receives, so this struct's size is ABI.
const SvcRegisterDesc = extern struct {
    version: u32 = SVC_REGISTER_DESC_VERSION,
    /// Endpoint clients send requests to.
    service_endpoint: u32 = 0,
    /// Reserved, 0.
    flags: u32 = 0,
    /// NUL-terminated service name.
    name: [SVC_NAME_MAX]u8 = [_]u8{0} ** SVC_NAME_MAX,
    /// Provider instance index within the class.
    instance: u32 = 0,
    /// NUL-terminated class name; all-zero means no class.
    class_name: [SVC_CLASS_MAX]u8 = [_]u8{0} ** SVC_CLASS_MAX,
};

/// Label and canonical-id capacities of the block descriptor, both INCLUDING
/// the terminating NUL. Equal to BLOCK_DESCRIPTOR_LABEL_MAX /
/// BLOCK_DESCRIPTOR_ID_MAX in `abi/constants.yaml`.
pub const BLOCK_LABEL_MAX: usize = 144;
pub const BLOCK_ID_MAX: usize = 64;
pub const BLOCK_DESCRIPTOR_VERSION: u32 = 1;

/// One block device, mirroring `wasmos_block_descriptor_t` in
/// `src/drivers/include/wasmos_driver_abi.h`. Carried in a transfer buffer by
/// BLOCK_IPC_IDENTIFY_RESP and DEVMGR_PUBLISH_BLOCK_DEVICE.
///
/// The C side is `__attribute__((packed))` and this one is not, which is safe
/// only because the layout was chosen so the two coincide: every field sits at
/// its natural alignment and the total is a multiple of 8, so neither compiler
/// inserts padding. The assertions below are what keep that true — a field added
/// on one side alone shifts an offset and fails the build here rather than
/// silently misreading a peer's bytes.
pub const BlockDescriptor = extern struct {
    version: u32 = BLOCK_DESCRIPTOR_VERSION,
    /// BLOCK_BACKEND_*
    backend: u32 = 0,
    /// Backend-local device number.
    unit: u32 = 0,
    /// 0 = whole device, else partition-table slot.
    partition: u32 = 0,
    /// PARTITION_SCHEME_*
    scheme: u32 = 0,
    /// FS_TYPE_*, from a superblock probe.
    fs_type: u32 = 0,
    sector_bytes: u32 = 0,
    /// BLOCK_DESCRIPTOR_FLAG_*
    flags: u32 = 0,
    /// Absolute on the underlying device.
    lba_start: u64 = 0,
    lba_count: u64 = 0,
    /// GPT partition type; zero unless scheme is GPT.
    type_guid: [16]u8 = [_]u8{0} ** 16,
    /// PARTUUID; zero unless scheme is GPT.
    part_guid: [16]u8 = [_]u8{0} ** 16,
    /// MBR partition type byte; zero unless scheme is MBR.
    mbr_type: u8 = 0,
    reserved: [7]u8 = [_]u8{0} ** 7,
    /// PARTLABEL, UTF-8, NUL-terminated.
    label: [BLOCK_LABEL_MAX]u8 = [_]u8{0} ** BLOCK_LABEL_MAX,
    /// NUL-terminated canonical id; the device's identity.
    canonical_id: [BLOCK_ID_MAX]u8 = [_]u8{0} ** BLOCK_ID_MAX,
};

comptime {
    if (@sizeOf(BlockDescriptor) != 88 + BLOCK_LABEL_MAX + BLOCK_ID_MAX) {
        @compileError("BlockDescriptor size disagrees with wasmos_block_descriptor_t");
    }
    if (@offsetOf(BlockDescriptor, "lba_start") != 32) @compileError("lba_start moved");
    if (@offsetOf(BlockDescriptor, "type_guid") != 48) @compileError("type_guid moved");
    if (@offsetOf(BlockDescriptor, "part_guid") != 64) @compileError("part_guid moved");
    if (@offsetOf(BlockDescriptor, "label") != 88) @compileError("label moved");
    if (@offsetOf(BlockDescriptor, "canonical_id") != 232) @compileError("canonical_id moved");
    // Fingerprints of two real canonical ids, pinned to the values the C
    // implementation produces. tests/unit/test_block_descriptor.c asserts the
    // same numbers from the other side, so the pair fails the build if either
    // implementation drifts -- which matters because these are the addresses a
    // filesystem uses to find its disk, and a silent divergence means it never
    // does.
    if (blockFingerprint("block:ata:0") != 4118534846) @compileError("fnv ata:0 mismatch");
    if (blockFingerprint("block:virtio-blk:8") != 2695290355) @compileError("fnv virtio:8 mismatch");
    if (blockFingerprint("") != 0) @compileError("fnv empty must be 0");
}

/// FNV-1a 32 over `id`: the `block` service class instance of a device whose
/// canonical id is that string. Must agree byte-for-byte with
/// wasmos_block_fingerprint() in `src/drivers/include/wasmos_driver_abi.h` —
/// the two compute the addresses C and Zig backends register under, and a
/// disagreement means a filesystem never finds its disk.
pub fn blockFingerprint(id: []const u8) u32 {
    if (id.len == 0) return 0;
    var hash: u32 = 2166136261; // FNV offset basis
    for (id) |c| {
        hash ^= c;
        hash = hash *% 16777619; // FNV prime
    }
    return hash;
}

/// Startup contract, mirroring `wasmos_spawn_info_t`.
const SPAWN_INFO_MAGIC: u32 = 0x57535049; // 'WSPI'
const SpawnInfo = extern struct {
    magic: u32 = 0,
    version: u32 = 0,
    header_size: u32 = 0,
    proc_endpoint: u32 = 0,
    tty: u32 = 0,
    module_count: u32 = 0,
    module_index: u32 = 0,
    args_off: u32 = 0,
    args_len: u32 = 0,
};

/// Longest startup-argument blob a driver sees. The process manager already
/// truncates the blob to 255 bytes (WASMOS_STARTUP_ARGS_MAX), so this matches
/// rather than bounds it.
const STARTUP_ARGS_MAX: usize = 256;

var g_spawn_info: SpawnInfo = .{};
var g_startup_args: [STARTUP_ARGS_MAX]u8 = [_]u8{0} ** STARTUP_ARGS_MAX;
var g_startup_args_len: usize = 0;
var g_startup_loaded: bool = false;

/// Read this process's spawn info and startup arguments. Idempotent, and called
/// on demand by `procEndpoint` and `startupArgs`, so a driver never has to call
/// it -- unlike an application, a driver has no runtime entry that could.
fn loadStartup() void {
    if (g_startup_loaded) return;
    g_startup_loaded = true;

    const bid = abi.spawn_info_buffer();
    if (bid <= 0) return;
    if (abi.xfer_buffer_read(
        bid,
        @intCast(@intFromPtr(&g_spawn_info)),
        @intCast(@sizeOf(SpawnInfo)),
        0,
    ) != 0 or g_spawn_info.magic != SPAWN_INFO_MAGIC) {
        g_spawn_info = .{};
        return;
    }

    var n: usize = g_spawn_info.args_len;
    if (n == 0) return;
    if (n > STARTUP_ARGS_MAX - 1) n = STARTUP_ARGS_MAX - 1;
    if (abi.xfer_buffer_read(
        bid,
        @intCast(@intFromPtr(&g_startup_args[0])),
        @intCast(n),
        @intCast(g_spawn_info.args_off),
    ) != 0) return;
    g_startup_args[n] = 0;
    g_startup_args_len = n;
}

/// IPC endpoint of the process manager, or a negative value when this process
/// carries no spawn info (which for a driver means it was not launched by the
/// process manager and cannot register anything).
pub fn procEndpoint() i32 {
    loadStartup();
    if (g_spawn_info.magic != SPAWN_INFO_MAGIC) return -1;
    return @bitCast(g_spawn_info.proc_endpoint);
}

/// The startup-argument blob the process manager passed at spawn time, empty
/// when there was none. For a PCI-rule spawn this is the matched device's
/// identity -- see `findArg`.
pub fn startupArgs() []const u8 {
    loadStartup();
    return g_startup_args[0..g_startup_args_len];
}

/// Value of the `key=` token in a space-separated argument blob, or null when
/// the key is absent. The value runs to the next space or the end of the blob.
///
/// This is how a PCI-matched driver learns which device it got: the device
/// manager packs the matched BAR/IRQ identity into the startup arguments
/// (`pci=00:04.0 vendor=1af4 device=1001 io=c040 irq=0b`) because a driver's
/// I/O grant is scoped to its device's window and does not reach the PCI
/// configuration ports a fresh bus scan would need.
pub fn findArg(args: []const u8, key: []const u8) ?[]const u8 {
    var i: usize = 0;
    while (i < args.len) {
        while (i < args.len and args[i] == ' ') i += 1;
        if (i >= args.len) return null;
        var end = i;
        while (end < args.len and args[end] != ' ') end += 1;
        const token = args[i..end];
        if (token.len > key.len and startsWith(token, key)) return token[key.len..];
        i = end;
    }
    return null;
}

fn startsWith(haystack: []const u8, needle: []const u8) bool {
    if (haystack.len < needle.len) return false;
    for (needle, 0..) |ch, i| {
        if (haystack[i] != ch) return false;
    }
    return true;
}

/// Parse exactly `digits` hexadecimal characters, or null when any of them is
/// not a hex digit or the slice is too short. Used to decode the fixed-width
/// fields of the PCI identity above, where a short value is a malformed
/// argument rather than a smaller number.
pub fn parseHex(s: []const u8, digits: usize) ?u32 {
    if (s.len < digits or digits == 0) return null;
    var value: u32 = 0;
    for (s[0..digits]) |ch| {
        const nibble: u32 = switch (ch) {
            '0'...'9' => ch - '0',
            'a'...'f' => 10 + (ch - 'a'),
            'A'...'F' => 10 + (ch - 'A'),
            else => return null,
        };
        value = (value << 4) | nibble;
    }
    return value;
}

// --- console ---------------------------------------------------------------

/// One line of driver log output, built in place and written by `end`.
///
/// A driver's only diagnostic channel is the serial console, and it has no
/// allocator and no formatter. This is the whole of both: append pieces, then
/// `end`. Content past the buffer is dropped rather than growing it or
/// splitting the line, so a log call can never fail or block.
pub const Line = struct {
    buf: [192]u8 = undefined,
    n: usize = 0,

    pub fn str(self: *Line, s: []const u8) *Line {
        for (s) |ch| {
            if (self.n >= self.buf.len) return self;
            self.buf[self.n] = ch;
            self.n += 1;
        }
        return self;
    }

    /// Append `v` in decimal.
    pub fn dec(self: *Line, v: u64) *Line {
        var digits: [20]u8 = undefined;
        var count: usize = 0;
        var rest = v;
        while (true) {
            digits[count] = @intCast('0' + (rest % 10));
            count += 1;
            rest /= 10;
            if (rest == 0) break;
        }
        while (count > 0) {
            count -= 1;
            _ = self.str(digits[count .. count + 1]);
        }
        return self;
    }

    /// Append `v` as exactly `digits` hexadecimal characters, zero-padded, with
    /// no `0x` prefix -- the register and identity values a driver logs are read
    /// against a fixed field width.
    pub fn hex(self: *Line, v: u64, digits: usize) *Line {
        const table = "0123456789ABCDEF";
        var i = digits;
        while (i > 0) {
            i -= 1;
            const nibble: usize = @intCast((v >> @intCast(i * 4)) & 0xF);
            _ = self.str(table[nibble .. nibble + 1]);
        }
        return self;
    }

    /// Terminate the line and write it to the console. The Line is reset, so it
    /// may be reused for the next line.
    pub fn end(self: *Line) void {
        if (self.n < self.buf.len) {
            self.buf[self.n] = '\n';
            self.n += 1;
        } else {
            self.buf[self.buf.len - 1] = '\n';
        }
        _ = abi.console_write(@intCast(@intFromPtr(&self.buf[0])), @intCast(self.n));
        self.n = 0;
    }
};

/// Write one complete line with no formatting.
pub fn log(s: []const u8) void {
    var line = Line{};
    _ = line.str(s);
    line.end();
}

// --- IPC -------------------------------------------------------------------

/// A decoded IPC message, in the layout the event loop delivers
/// (`coroutine.IpcMessage`). Declared here so `call` can return one without a
/// driver having to import the coroutine bindings for a bring-up request.
pub const Message = extern struct {
    type: i32 = 0,
    request_id: i32 = 0,
    arg0: i32 = 0,
    arg1: i32 = 0,
    arg2: i32 = 0,
    arg3: i32 = 0,
    source: i32 = 0,
    destination: i32 = 0,
};

fn lastMessage() Message {
    return .{
        .type = abi.ipc_last_field(0),
        .request_id = abi.ipc_last_field(1),
        .arg0 = abi.ipc_last_field(2),
        .arg1 = abi.ipc_last_field(3),
        .arg2 = abi.ipc_last_field(6),
        .arg3 = abi.ipc_last_field(7),
        .source = abi.ipc_last_field(4),
        .destination = abi.ipc_last_field(5),
    };
}

/// Send one message, yielding and retrying while the destination queue is full.
/// Returns 0, or the kernel's negative send status.
pub fn send(dest: i32, src: i32, msg_type: i32, request_id: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32) i32 {
    var tries: u32 = 0;
    while (true) {
        const rc = abi.ipc_send(dest, src, msg_type, request_id, arg0, arg1, arg2, arg3);
        if (rc == 0) return 0;
        tries += 1;
        if (rc != IPC_ERR_FULL or tries >= IPC_SEND_RETRY_LIMIT) return rc;
        _ = abi.sched_yield();
    }
}

/// Synchronous request/reply, for BRING-UP ONLY: send from `reply_ep` to
/// `dest`, then BLOCK on `reply_ep` until the matching reply arrives.
///
/// A running driver must not call this. It is here for the window before the
/// coroutine runtime exists -- `prepare` resolving a bus service, claiming an
/// MSI vector, registering the service -- where there is no future to await on
/// because there is no runtime to await in. Once the runtime is pumping, a
/// request belongs on the event loop as an IpcFuture, which is why the drain
/// and select helpers a hand-rolled loop would need are deliberately NOT part
/// of this module.
///
/// Only a message carrying this `request_id` AND sent from `dest` is accepted;
/// every other message that lands on `reply_ep` meanwhile is consumed and
/// DISCARDED. `reply_ep` must therefore be a private reply endpoint, never a
/// live service endpoint. There is no timeout: a peer that never answers blocks
/// the caller indefinitely.
pub fn call(dest: i32, reply_ep: i32, msg_type: i32, request_id: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32) ?Message {
    if (send(dest, reply_ep, msg_type, request_id, arg0, arg1, arg2, arg3) != 0) return null;
    while (true) {
        if (abi.ipc_select_one(reply_ep) < 0) return null;
        if (abi.ipc_last_field(1) != request_id) continue;
        if (abi.ipc_last_field(4) != dest) continue;
        return lastMessage();
    }
}

// --- transfer buffers ------------------------------------------------------

/// Copy `bytes` into the transfer buffer `buffer_id` at `offset`. The buffer is
/// either this process's own or one a client borrowed to it; the kernel admits
/// the write on the strength of that grant. Returns true on success.
pub fn bufferWrite(buffer_id: i32, bytes: []const u8, offset: u32) bool {
    if (bytes.len == 0) return true;
    return abi.xfer_buffer_write(
        buffer_id,
        @intCast(@intFromPtr(&bytes[0])),
        @intCast(bytes.len),
        @intCast(offset),
    ) == 0;
}

/// Acquire a transfer buffer of at least `len` bytes and fill it with `bytes`
/// at offset 0. Returns the owned buffer id, which the caller must release with
/// `bufferRelease` once the request that names it has completed.
fn bufferStage(bytes: []const u8) ?i32 {
    const bid = abi.xfer_buffer_acquire(@intCast(bytes.len));
    if (bid < 0) return null;
    if (!bufferWrite(bid, bytes, 0)) {
        _ = abi.xfer_buffer_release(bid);
        return null;
    }
    return bid;
}

pub fn bufferRelease(buffer_id: i32) void {
    _ = abi.xfer_buffer_release(buffer_id);
}

/// Grant flags for `bufferBorrow`, matching WASMOS_BUFFER_GRANT_* in
/// `src/drivers/include/wasmos_driver_abi.h`.
pub const BUFFER_GRANT_READ: i32 = 0x1;
pub const BUFFER_GRANT_WRITE: i32 = 0x2;

/// Acquire a transfer buffer of at least `len` bytes, left zeroed. The caller
/// owns it and must release it with `bufferRelease`.
pub fn bufferAcquire(len: usize) ?i32 {
    const bid = abi.xfer_buffer_acquire(@intCast(len));
    return if (bid < 0) null else bid;
}

/// Lend this process's buffer `buffer_id` to whoever owns `grantee_endpoint`
/// with `flags` rights, returning the grantee's borrow id.
///
/// Each call allocates its own borrow slot, so a server that re-grants the same
/// buffer to the same client on every request accumulates them; grant once per
/// client and let it address the buffer by object id afterwards, which any
/// grantee may do.
///
/// A borrow is held per CONTEXT: the kernel resolves the endpoint to its owning
/// process and allows one active borrow per object per process, so granting the
/// same buffer to a second endpoint of a client that already holds one returns
/// ALREADY_BORROWED. That is a sign the buffer is on the wrong side of the
/// exchange -- the CLIENT owns a transfer buffer and lends it to the server for
/// one request (docs/architecture/12-dma-transfers.md), which is the shape that
/// needs no such bookkeeping.
pub fn bufferBorrow(grantee_endpoint: i32, buffer_id: i32, flags: i32) ?i32 {
    const borrow = abi.xfer_buffer_borrow(grantee_endpoint, buffer_id, flags);
    return if (borrow < 0) null else borrow;
}

// --- service registry ------------------------------------------------------

/// Reply endpoint for the registry handshakes below, created once and reused.
///
/// It is deliberately not the service endpoint: `call` matches one reply and
/// discards everything else that arrives meanwhile, so running it on a live
/// service endpoint would silently drop a client's first request.
var g_registry_reply_ep: i32 = -1;

/// The private reply endpoint above, for a driver's own synchronous requests
/// during bring-up -- resolving a bus service, programming an MSI vector. It is
/// the endpoint those calls belong on for the same reason the registry
/// handshakes use it, and sharing one costs nothing because bring-up is
/// sequential. Negative when no endpoint could be created.
pub fn privateReplyEndpoint() i32 {
    return registryReplyEndpoint();
}

fn registryReplyEndpoint() i32 {
    if (g_registry_reply_ep < 0) g_registry_reply_ep = abi.ipc_create_endpoint();
    return g_registry_reply_ep;
}

/// Pack up to 16 characters of a service name into four IPC argument words,
/// four bytes each, little end first. Unused positions stay zero, so a shorter
/// name is zero-padded and a 16-character one travels without a terminator;
/// anything longer is truncated to its first 16 bytes. Mirrors
/// wasmos_ipc_pack_name16.
fn packName16(name: []const u8) [4]i32 {
    var args = [4]i32{ 0, 0, 0, 0 };
    var i: usize = 0;
    while (i < name.len and i < 16) : (i += 1) {
        const slot = i / 4;
        const shift: u5 = @intCast((i % 4) * 8);
        args[slot] |= @as(i32, name[i]) << shift;
    }
    return args;
}

/// Resolve a service by name through the process manager, retrying `attempts`
/// times and yielding between tries so a service that has not registered yet
/// gets a chance to run. Returns its endpoint, or null once the attempts are
/// exhausted. Each attempt consumes one request id from `request_id_base`.
pub fn lookupService(proc_endpoint: i32, name: []const u8, request_id_base: i32, attempts: i32) ?i32 {
    const reply_ep = privateReplyEndpoint();
    if (reply_ep < 0) return null;
    const args = packName16(name);
    const tries = if (attempts > 0) attempts else 1;
    var i: i32 = 0;
    while (i < tries) : (i += 1) {
        if (call(
            proc_endpoint,
            reply_ep,
            op.SVC_IPC_LOOKUP_REQ,
            request_id_base + i,
            args[0],
            args[1],
            args[2],
            args[3],
        )) |reply| {
            // The registry reports "no such service" as an all-ones endpoint,
            // which is a valid reply rather than a failed call.
            if (reply.type == op.SVC_IPC_LOOKUP_RESP and @as(u32, @bitCast(reply.arg0)) != 0xFFFFFFFF) {
                return reply.arg0;
            }
        }
        // Bring-up only, and bounded: a service that has not registered yet
        // needs the scheduler to run it before the next attempt can succeed.
        _ = abi.sched_yield();
    }
    return null;
}

// --- message-signalled interrupts ------------------------------------------

/// The interrupt-controller address/data pair that makes a device raise a
/// vector, plus the vector itself. Layout mirrors `wasmos_msi_desc_t`.
///
/// A driver forwards the pair verbatim to the bus driver that owns config space
/// and never interprets it: the kernel owns the vector namespace but never
/// touches the device, and the bus driver touches the device but does not
/// allocate vectors.
pub const MsiDesc = extern struct {
    address_lo: u32 = 0,
    address_hi: u32 = 0,
    data: u32 = 0,
    vector: u32 = 0,
};

/// Allocate one MSI vector and bind it to `endpoint`, which must be owned by
/// the caller. Returns the descriptor to hand to the bus driver, or null on
/// failure. Requires the `irq.route` capability.
///
/// Unlike an IRQ line an MSI vector is edge-triggered and exclusively owned, so
/// its events need no `irqAck` and the vector is never masked -- none of the
/// shared-line ceremony applies. Events arrive as MSI_EVENT_TYPE with arg0 set
/// to the table entry the driver programmed, i.e. which of its own interrupt
/// sources fired.
pub fn msiAlloc(endpoint: i32) ?MsiDesc {
    var desc = MsiDesc{};
    if (abi.msi_alloc(endpoint, @intCast(@intFromPtr(&desc))) != 0) return null;
    return desc;
}

/// Release an MSI vector allocated by this process. Mask the device's table
/// entry first (the bus driver's unbind), or the device can raise a vector that
/// has been handed to someone else.
pub fn msiFree(vector: u32) void {
    _ = abi.msi_free(@intCast(vector));
}

/// IPC message type the kernel sends for an MSI event, mirroring
/// WASMOS_IPC_MSI_EVENT_TYPE.
pub const MSI_EVENT_TYPE: i32 = 0xFF01;

fn copyName(dst: []u8, name: []const u8) void {
    var i: usize = 0;
    while (i + 1 < dst.len and i < name.len and name[i] != 0) : (i += 1) dst[i] = name[i];
    dst[i] = 0;
}

/// Publish `service_endpoint` to the process manager under the concrete name
/// `name` and, when `class_name` is non-empty, under the virtual class as
/// provider `instance`. Returns the assigned service handle, or null on
/// failure.
///
/// Prefer a class for a backend-neutral driver: a client that looks the class up
/// finds whichever provider is present without naming this driver. Claiming a
/// class requires the `svc.class` capability in the manifest -- the process
/// manager gates the claim on it.
pub fn registerService(proc_endpoint: i32, service_endpoint: i32, name: []const u8, class_name: []const u8, instance: u32, request_id: i32) ?i32 {
    const reply_ep = registryReplyEndpoint();
    if (reply_ep < 0) return null;

    var desc = SvcRegisterDesc{ .service_endpoint = @bitCast(service_endpoint), .instance = instance };
    copyName(&desc.name, name);
    copyName(&desc.class_name, class_name);

    const raw: [*]const u8 = @ptrCast(&desc);
    const bid = bufferStage(raw[0..@sizeOf(SvcRegisterDesc)]) orelse return null;
    defer bufferRelease(bid);

    const reply = call(
        proc_endpoint,
        reply_ep,
        op.SVC_IPC_REGISTER_DESC_REQ,
        request_id,
        0,
        @sizeOf(SvcRegisterDesc),
        bid,
        0,
    ) orelse return null;
    if (reply.type != op.SVC_IPC_REGISTER_RESP) return null;
    return reply.arg0;
}

/// Tell the process manager this driver is ready to serve, and wait for the
/// acknowledgement.
///
/// The wait is load-bearing, not politeness: a parent doing a synchronous spawn
/// is blocked until the process manager has marked this child ready, so a
/// fire-and-forget notification would race the parent's launch step.
pub fn notifyReady(proc_endpoint: i32) void {
    const reply_ep = registryReplyEndpoint();
    if (reply_ep < 0) return;
    _ = call(proc_endpoint, reply_ep, op.PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0);
}

// --- device resources ------------------------------------------------------

/// A pinned, contiguous DMA region, mapped into this module's linear memory.
pub const Region = struct {
    /// The module's own view of the region. A device also reads and writes this
    /// memory, so take a volatile view of any field the device may change under
    /// the driver -- a completion status byte, a ring index -- rather than
    /// reading it through this pointer directly.
    base: [*]u8,
    /// The address the DEVICE uses for the same memory.
    phys: u64,
    bytes: u64,
};

/// Allocate `pages` of driver-owned, pinned, contiguous memory below 2 GiB and
/// map it into this module's linear memory. Returns null on failure, which
/// includes having no `dma.buffer` capability or a budget too small for the
/// request.
///
/// The mapping is a real page remap, so writes through `base` reach the exact
/// physical pages the device reads. The region is placed ABOVE the module's
/// live data, which is why a DMA-capable driver must declare enough linear
/// memory in its manifest's `[link]` section to contain the window.
pub fn regionAlloc(pages: u32, cache_policy: i32) ?Region {
    var phys: u64 = 0;
    const offset = abi.region_alloc(
        @intCast(pages),
        cache_policy,
        @intCast(@intFromPtr(&phys)),
    );
    if (offset < 0) return null;
    return .{
        .base = @ptrFromInt(@as(usize, @intCast(offset))),
        .phys = phys,
        .bytes = @as(u64, pages) * 4096,
    };
}

/// A device's I/O-port window, based at the address the spawn profile granted.
///
/// Every access is capability-checked by the kernel against the window the
/// driver holds, so naming a port outside it fails rather than reaching a
/// neighbouring device. Reads return the device's float value (all ones) when
/// the kernel refuses, because a port read has no value that could encode a
/// failure -- 0xFFFF is exactly what an absent device presents, so a caller that
/// already handles an absent device handles this too.
pub const Ports = struct {
    base: u16,

    /// Absolute port for `offset` within the window. The sum is taken in i32
    /// because that is the host call's parameter type, and because a window
    /// near the top of the port space would otherwise wrap in u16 arithmetic
    /// and silently address the bottom of it.
    fn port(self: Ports, offset: u16) i32 {
        return @as(i32, self.base) + @as(i32, offset);
    }

    pub fn in8(self: Ports, offset: u16) u8 {
        var value: u32 = 0xFF;
        if (abi.io_in8(self.port(offset), @intCast(@intFromPtr(&value))) != 0) return 0xFF;
        return @intCast(value & 0xFF);
    }

    pub fn in16(self: Ports, offset: u16) u16 {
        var value: u32 = 0xFFFF;
        if (abi.io_in16(self.port(offset), @intCast(@intFromPtr(&value))) != 0) return 0xFFFF;
        return @intCast(value & 0xFFFF);
    }

    pub fn in32(self: Ports, offset: u16) u32 {
        var value: u32 = 0xFFFFFFFF;
        if (abi.io_in32(self.port(offset), @intCast(@intFromPtr(&value))) != 0) return 0xFFFFFFFF;
        return value;
    }

    pub fn out8(self: Ports, offset: u16, value: u8) void {
        _ = abi.io_out8(self.port(offset), value);
    }

    pub fn out16(self: Ports, offset: u16, value: u16) void {
        _ = abi.io_out16(self.port(offset), value);
    }

    pub fn out32(self: Ports, offset: u16, value: u32) void {
        _ = abi.io_out32(self.port(offset), @bitCast(value));
    }
};

/// Deliver interrupts on `irq_line` to `endpoint` as IPC. Returns true when the
/// line is routed. Requires the `irq.route` capability.
pub fn irqRoute(irq_line: u32, endpoint: i32) bool {
    return abi.irq_route_ipc(@intCast(irq_line), endpoint) == 0;
}

/// Acknowledge and re-arm `irq_line` after handling an interrupt.
///
/// The kernel keeps a dispatched line masked until EVERY driver sharing it has
/// acked, so skipping this stalls the other sharers' interrupts as well as this
/// driver's. On a level-triggered PCI line the device must ALSO be made to stop
/// asserting -- for virtio that is reading the ISR register -- or the line
/// re-fires immediately on unmask.
pub fn irqAck(irq_line: u32) void {
    _ = abi.irq_ack(@intCast(irq_line));
}

/// Map `length` bytes at `offset` of a client's borrowed transfer buffer for the
/// device to access directly, returning the device address. Null means the
/// mapping was refused and the caller must fall back to staging the data
/// through its own memory.
///
/// The borrow is what makes this admissible: it is the client's own grant, so
/// the mapping cannot reach memory the client did not offer, and the kernel
/// bounds the range.
pub fn dmaMapBorrow(borrow_id: i32, offset: u32, length: u32, direction: i32) ?u32 {
    const addr = abi.dma_map_borrow(borrow_id, @intCast(offset), @intCast(length), direction);
    if (addr <= 0) return null;
    return @intCast(addr);
}

/// Make the device's writes visible to the client (or the client's writes
/// visible to the device) over `[offset, offset+length)` of the MAPPING -- the
/// offset is relative to where `dmaMapBorrow` started, not to the buffer.
pub fn dmaSyncBorrow(borrow_id: i32, offset: u32, length: u32, op_direction: i32) void {
    _ = abi.dma_sync_borrow(borrow_id, @intCast(offset), @intCast(length), op_direction);
}

pub fn dmaUnmapBorrow(borrow_id: i32) void {
    _ = abi.dma_unmap_borrow(borrow_id);
}
