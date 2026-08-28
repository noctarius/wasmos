//! virtio_blk.zig — VirtIO block-device driver, in Zig, on the coroutine runtime.
//!
//! Serves the block-device IPC interface (BLOCK_IPC_IDENTIFY_REQ,
//! BLOCK_IPC_READ_REQ, BLOCK_IPC_WRITE_REQ, BLOCK_IPC_READ_ZC_REQ) on top of a
//! legacy virtio-blk PCI device, so a filesystem driver reads a virtio disk
//! through exactly the protocol it reads an ATA disk through.
//!
//! Shape: an async service, not a blocking loop
//! -------------------------------------------
//! The module's entry is `async_initialize` (libsys), which owns the reply
//! endpoint, the event loop and the coroutine runtime, and pumps them:
//!
//!     while (root is alive) { runtime.runBudget(1); if (root waiting) loop.poll(1); }
//!
//! So this file supplies two things and no loop of its own: `prepare`, which
//! brings the device up before the runtime starts, and `rootTask`, a stackless
//! coroutine that parks on a future whenever it has nothing to do. `poll` parks
//! on the loop's select set, so an idle driver sleeps rather than spins.
//!
//! ONE endpoint carries everything. The loop drains a single receiver endpoint
//! and demultiplexes: a reply matching an in-flight request id settles its
//! future, anything else reaches the default handler. So the block service is
//! registered ON that endpoint and the interrupt is routed TO it, and
//! `onMessage` sorts client requests from completion interrupts. That is what
//! the loop is built for, and it is why the driver needs no select set, no
//! drain loop and no timed wait of its own.
//!
//! Concurrency lives in the root task, deliberately. Spawning a coroutine per
//! request would be the obvious shape and is WRONG under this runner: it polls
//! whenever the ROOT is parked, and poll blocks until an event arrives, so a
//! ready child task would sit unresumed until an unrelated message happened to
//! come in. Instead the root task owns a slot table and keeps up to
//! MAX_INFLIGHT chains on the queue at once, parking only when every slot is
//! idle.
//!
//! What virtio-blk is
//! ------------------
//! One virtqueue (the requestq, index 0). A request is a THREE-descriptor
//! chain, which is the reason `vring_alloc_chain` exists:
//!
//!     [ 16-byte header  device-readable ]  type, ioprio, sector
//!     [ data            r or w          ]  512 * count bytes
//!     [ 1-byte status   device-writable ]  0 = OK
//!
//! The direction of the middle descriptor is the whole difference between a
//! read and a write: for VIRTIO_BLK_T_IN the device writes it, for
//! VIRTIO_BLK_T_OUT the device reads it.
//!
//! Zero copy, and why the data descriptor is not this driver's memory
//! -----------------------------------------------------------------
//! BLOCK_IPC_READ_REQ names the CLIENT's block buffer by physical address, and
//! that address goes straight into the data descriptor: the device transfers
//! into the requester's pages and no byte is copied by any CPU. The same holds
//! for BLOCK_IPC_READ_ZC_REQ, where the client's borrow of a transfer buffer is
//! mapped for the device instead. Only the per-slot header and status byte live
//! in this driver's own pinned region, and together they are 17 bytes.
//!
//! Interrupts
//! ----------
//! Completion is message-signalled where the device and `pci-bus` allow it. That
//! is not a latency optimisation: this driver would otherwise be the system's
//! only INTx consumer, on a line it shares with the PIIX4 power-management
//! bridge, and an assertion nobody services re-fires on every unmask. An MSI-X
//! vector is edge-triggered and nobody else's. INTx routing remains as the
//! fallback for a device or bus service that cannot provide one.
//!
//! Enabling MSI-X inserts two vector registers at 0x14/0x16 and SHIFTS the
//! device-specific configuration from 0x14 to 0x18, which is why the capacity
//! read goes through `configBase()` rather than a constant.
//!
//! Service identity
//! ----------------
//! Registered as the concrete name "virtio-blk" under the virtual class
//! "block", so a client discovers it by class without naming this driver, and
//! without competing for the plain "block" name the ATA driver holds for the
//! boot disk.
//!
//! A class instance is a DISK, and its number is (backend << 8) | unit. One
//! virtio-blk device is one disk, so the unit identifies the DEVICE: it comes
//! from the device's place on the bus, not from a counter, so two virtio-blk
//! devices get different units instead of colliding on one instance. The number
//! is derived rather than allocated, so it is the same every boot whatever
//! order the drivers probed in, and a client can decode it back into the pair a
//! device-manager rule names with DRIVER== and ATTR{unit}.
//!
//! The same disk is published to the device-manager inventory, which is what
//! makes it visible to a block rule at all -- without that a filesystem cannot
//! be mounted on it, however well the driver works.

const driver = @import("driver.zig");
const vring = @import("vring.zig");
const co = @import("coroutine.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");
const abi = @import("wasmos_constants.zig");

/// `WASMOS_PCI_MSI_KIND_MSIX`, as PCI_IPC_MSI_QUERY reports it in arg0. MSI-X
/// wins when a device offers both: it addresses each vector independently,
/// where plain MSI needs one aligned block of consecutive vectors.
const PCI_MSI_KIND_MSIX: i32 = 2;

/// IPC message types the kernel sends for an interrupt: 0xFF00 for a routed
/// INTx line (an ack is owed) and 0xFF01 for an MSI vector (none is).
const IPC_IRQ_EVENT_TYPE: i32 = 0xFF00;
const IPC_MSI_EVENT_TYPE: i32 = 0xFF01;

/// 0x1AF4 is the Red Hat / virtio vendor id. Both device ids below identify a
/// block device and are accepted interchangeably: 0x1001 is the transitional
/// (legacy) id, 0x1042 is 0x1040 + virtio device type 2.
const PCI_VENDOR_VIRTIO: u32 = 0x1AF4;
const PCI_DEVICE_BLK_LEGACY: u32 = 0x1001;
const PCI_DEVICE_BLK_MODERN: u32 = 0x1042;

/// Legacy virtqueue registers, as offsets into the device's I/O window.
const REG_DEVICE_FEATURES: u16 = 0x00;
const REG_DRIVER_FEATURES: u16 = 0x04;
/// u32: ring page frame number (physical address >> 12).
const REG_QUEUE_PFN: u16 = 0x08;
/// u16: size of the selected queue; 0 means the queue does not exist.
const REG_QUEUE_SIZE: u16 = 0x0C;
/// u16: selects which queue the registers above address.
const REG_QUEUE_SELECT: u16 = 0x0E;
/// u16 doorbell: write the queue index.
const REG_QUEUE_NOTIFY: u16 = 0x10;
const REG_DEVICE_STATUS: u16 = 0x12;
/// Reading this de-asserts the device's level-triggered INTx line.
const REG_ISR_STATUS: u16 = 0x13;
/// Vector-selection registers, which EXIST ONLY once MSI-X is enabled, and the
/// value the device reads back when it refuses a binding.
const REG_MSIX_CONFIG_VECTOR: u16 = 0x14;
const REG_MSIX_QUEUE_VECTOR: u16 = 0x16;
const MSIX_NO_VECTOR: u16 = 0xFFFF;
/// One interrupt source, one table entry: the request queue's completion.
const MSIX_ENTRY_QUEUE: u16 = 0;

/// Base of `struct virtio_blk_config`. Enabling MSI-X inserts the two vector
/// registers above at 0x14 and pushes the configuration to 0x18, so the base is
/// a function of the interrupt mode, never a constant -- see `configBase`.
const REG_DEVICE_CONFIG_INTX: u16 = 0x14;
const REG_DEVICE_CONFIG_MSIX: u16 = 0x18;

/// Fields of `struct virtio_blk_config`, as offsets from `configBase()`.
/// `capacity` is the device size in 512-byte sectors and is the only field this
/// driver reads unconditionally -- the rest are gated on feature bits.
const CFG_CAPACITY_LO: u16 = 0x00;
const CFG_CAPACITY_HI: u16 = 0x04;
const CFG_BLK_SIZE: u16 = 0x14;

/// Device-status bits, ORed into REG_DEVICE_STATUS in ascending order as
/// bring-up progresses. The bits accumulate; FAILED reports that the driver
/// gave up, and writing 0 resets the device.
const STATUS_ACK: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FAILED: u8 = 128;

/// Feature bits this driver reads. RO is the only one it must honour -- writing
/// a read-only device is refused rather than attempted. BLK_SIZE advertises a
/// logical block size other than 512, which this driver does not implement, so
/// it is deliberately NOT accepted: leaving the bit unset keeps the device on
/// the 512-byte sectors the block protocol is defined in.
const FEATURE_BLK_SIZE: u32 = 1 << 6;
const FEATURE_RO: u32 = 1 << 5;
/// The device supports VIRTIO_BLK_T_FLUSH. Without it the device has no volatile
/// write cache to commit, so a flush request is unnecessary rather than
/// unavailable (virtio 1.2, 5.2.4).
const FEATURE_FLUSH: u32 = 1 << 9;

/// Request types, and the status byte the device writes back.
const REQ_TYPE_IN: u32 = 0; // device -> memory (a disk read)
const REQ_TYPE_OUT: u32 = 1; // memory -> device (a disk write)
const REQ_TYPE_FLUSH: u32 = 4; // commit the device's write cache to media
const REQ_STATUS_OK: u8 = 0;
/// Sentinel written into a slot's status byte before its chain is published.
/// The device overwrites it, so seeing it after a completion means the device
/// reported the chain without touching its status -- a device bug, reported as
/// an I/O error rather than mistaken for success.
const REQ_STATUS_UNSET: u8 = 0xFF;

/// This driver serves exactly one disk, so its backend-local unit identifies the
/// DEVICE rather than a drive on it -- and it is taken from where the device
/// sits on the bus, `(slot << 3) | function`, which is the same packing the
/// BDF uses and fits a unit's eight bits exactly (5 bits of slot, 3 of
/// function).
///
/// A constant 0 would have been simpler and was wrong: a second virtio-blk
/// device would claim the same unit, and therefore the same `block` class
/// instance, and its registration would be refused. The bus address is
/// intrinsic to the device and stable for a machine configuration, so it does
/// not depend on which driver probed first -- which is the whole reason the
/// identity is derived rather than allocated.
///
/// TODO: the bus number is dropped, so two devices at the same slot on
/// different buses still collide. A unit is a byte, so carrying the bus needs
/// the device manager's record to widen -- the same change that would let two
/// IDE controllers be told apart.
fn blockUnit() u32 {
    return ((g_dev.slot & 0x1F) << 3) | (g_dev.function & 0x07);
}

fn blockClassInstance() u32 {
    return (@as(u32, @intCast(abi.BLOCK_BACKEND_VIRTIO_BLK)) << 8) | blockUnit();
}

/// virtio-blk defines exactly one queue, index 0, the requestq. MAX_QUEUE caps
/// the queue size this driver accepts from the device.
const REQUEST_QUEUE: u16 = 0;
const MAX_QUEUE: u16 = 256;

/// Fixed 512-byte sector, which is what the block IPC protocol is defined in.
const SECTOR_BYTES: u32 = 512;
/// Sectors one request may move. Matches WASMOS_BLOCK_ZC_MAX_SECTORS, which is
/// what a zero-copy client may ask for, so the two paths accept the same
/// requests.
const MAX_SECTORS: u32 = 8;
/// Packing of BLOCK_IPC_READ_ZC_REQ's arg2, mirroring
/// WASMOS_BLOCK_ZC_BORROW_SHIFT / WASMOS_BLOCK_ZC_COUNT_MASK.
const ZC_BORROW_SHIFT: u5 = 12;
const ZC_COUNT_MASK: i32 = 0xFFF;

/// Requests that may be on the queue at once. Each holds three descriptors and
/// one SLOT_STRIDE-byte span of the scratch page, so the ceiling is the scratch
/// page rather than the 256-entry ring.
const MAX_INFLIGHT: usize = 8;
/// Bytes of scratch per slot: a 16-byte header at the start and a status byte
/// at SLOT_STATUS_OFF. They are separate descriptors and only need to be
/// distinct; the gap keeps a device that overruns the header off the status.
const SLOT_STRIDE: u32 = 128;
const SLOT_STATUS_OFF: u32 = 64;

/// Idle poll interval, and how many of them an in-flight request may survive
/// before the device is declared lost. 8 x 250 ms is the same ~2 second ceiling
/// the driver's earlier blocking wait used.
///
/// The interval exists ONLY to age requests: completions arrive as interrupts,
/// so a driver with nothing outstanding would be woken for no reason. The loop
/// is therefore left parking indefinitely until the first request is accepted,
/// and put back that way once the last one retires -- see `armIdleTimer`.
const POLL_INTERVAL_MS: i32 = 250;
const MAX_IDLE_TICKS: u16 = 8;

/// The 16-byte request header, laid out as the device reads it.
const ReqHeader = extern struct {
    type: u32,
    ioprio: u32,
    sector: u64,
};

const Device = struct {
    present: bool = false,
    ready: bool = false,
    read_only: bool = false,
    /// VIRTIO_BLK_F_FLUSH was negotiated. When it was not, the device has no
    /// volatile write cache and a flush request is answered without one.
    flush_supported: bool = false,
    bus: u32 = 0,
    slot: u32 = 0,
    function: u32 = 0,
    device_id: u32 = 0,
    irq: u32 = 0,
    ports: driver.Ports = .{ .base = 0 },
    /// Device size in 512-byte sectors, clamped to what fits an IPC argument.
    capacity_sectors: u32 = 0,
    /// True when the device reports more sectors than an IPC argument can
    /// carry, so IDENTIFY reports a truncated capacity.
    capacity_truncated: bool = false,
    /// True once `pci-bus` has programmed the device's MSI-X table entry. The
    /// device then no longer drives INTx, and the register layout has shifted.
    msix_enabled: bool = false,
    /// Kernel vector behind table entry MSIX_ENTRY_QUEUE.
    msix_vector: u32 = 0,
    /// True while a routed INTx line is the completion source, so an ack is owed
    /// for every event.
    irq_routed: bool = false,
};

/// Where one request is in its life. A slot moves free -> pending -> in_flight
/// -> complete -> free. Only the root task advances it out of `pending` or
/// `complete`; only the message handler advances it out of `in_flight`.
const SlotState = enum { free, pending, in_flight, complete };

const Slot = struct {
    state: SlotState = .free,
    /// Who to answer, and with which request id.
    source: i32 = 0,
    request_id: i32 = 0,
    msg_type: i32 = 0,
    /// Decoded request.
    lba: u32 = 0,
    count: u32 = 0,
    /// Device address of the caller's data buffer.
    data_phys: u64 = 0,
    /// Borrow to unmap once the transfer is done; 0 for a non-zero-copy request.
    borrow: i32 = 0,
    borrow_bytes: u32 = 0,
    /// Head descriptor while the chain is on the queue.
    chain_head: u16 = 0,
    /// Idle intervals this request has survived, for the deadline in `rootTask`.
    idle_ticks: u16 = 0,
    /// Outcome to report, set when the completion is observed.
    result: i32 = 0,
};

var g_dev: Device = .{};
var g_pci_endpoint: i32 = -1;
var g_queue: vring.Queue = undefined;
var g_queue_ready: bool = false;
/// Pinned region holding each slot's request header and status byte. Everything
/// else a request touches is the client's memory.
var g_scratch: driver.Region = undefined;
var g_slots: [MAX_INFLIGHT]Slot = [_]Slot{.{}} ** MAX_INFLIGHT;

/// The future the root task parks on when every slot is idle, and the promise
/// the message and timeout handlers settle to wake it.
var g_wake_future: co.Future = .{};
var g_wake_promise: co.Promise = .{};
/// True while the root task is parked on `g_wake_future`, so a handler only
/// settles a promise someone is actually waiting on.
var g_root_parked: bool = false;

// --- async service contract ------------------------------------------------

/// Definition of one async WASM guest, mirroring `wasmos_sys_wasm_async_config_t`.
/// The runtime, root task, event loop and reply endpoint are scratch state the
/// libsys runner fills in; only `resume`, `prepare` and `user` are inputs.
const AsyncServiceConfig = extern struct {
    runtime: co.Runtime = .{},
    root: co.Coroutine = .{},
    event_loop: co.EventLoop = .{},
    reply_endpoint: i32 = 0,
    @"resume": ?co.TaskResume = null,
    prepare: ?*const fn (?*anyopaque, i32, i32, i32, i32) callconv(.c) void = null,
    user: ?*anyopaque = null,
};

/// The single global the libsys `async_initialize` entry runs. A driver defines
/// `wasmos_async_service`; an application would define `wasmos_async_app`.
export var wasmos_async_service: AsyncServiceConfig = .{
    .@"resume" = rootTask,
    .prepare = prepare,
};

fn loop() *co.EventLoop {
    return &wasmos_async_service.event_loop;
}

/// The endpoint the runner created: the loop's receiver, this driver's service
/// endpoint, and the interrupt's destination, all the same one. See the header.
fn endpoint() i32 {
    return wasmos_async_service.reply_endpoint;
}

/// Turn the loop's idle timer on while requests are outstanding and off again
/// when none are, so an idle driver parks indefinitely instead of waking on a
/// timer that has nothing to age.
fn armIdleTimer(on: bool) void {
    loop().poll_timeout_ms = if (on) POLL_INTERVAL_MS else 0;
}

// --- device access ---------------------------------------------------------

/// vring doorbell: tell the device the request queue has new available buffers.
fn notifyQueue(user: ?*anyopaque) callconv(.c) void {
    _ = user;
    g_dev.ports.out16(REG_QUEUE_NOTIFY, REQUEST_QUEUE);
}

fn setStatusBit(bits: u8) void {
    g_dev.ports.out8(REG_DEVICE_STATUS, bits);
}

/// Offset of `struct virtio_blk_config`, which moves when MSI-X is enabled.
/// Read it through here rather than caching it: it is only correct after the
/// MSI-X binding has either succeeded or failed.
fn configBase() u16 {
    return if (g_dev.msix_enabled) REG_DEVICE_CONFIG_MSIX else REG_DEVICE_CONFIG_INTX;
}

/// Read the device's 64-bit capacity as two 32-bit halves, which is how a
/// 32-bit guest must address it: no 64-bit value crosses a host-call boundary.
fn readCapacity() u64 {
    const lo: u64 = g_dev.ports.in32(configBase() + CFG_CAPACITY_LO);
    const hi: u64 = g_dev.ports.in32(configBase() + CFG_CAPACITY_HI);
    return (hi << 32) | lo;
}

// --- probe -----------------------------------------------------------------

fn isVirtioBlk(vendor_id: u32, device_id: u32) bool {
    if (vendor_id != PCI_VENDOR_VIRTIO) return false;
    return device_id == PCI_DEVICE_BLK_LEGACY or device_id == PCI_DEVICE_BLK_MODERN;
}

/// Take the device identity from the startup arguments the device manager packs
/// for a PCI-rule spawn. That is the only probe path: this driver's I/O grant is
/// scoped to its device's BAR and does not reach the PCI configuration ports a
/// bus scan would need. Returns false when the arguments name no usable device.
fn probeFromStartupArgs() bool {
    const args = driver.startupArgs();
    if (args.len == 0) return false;

    const pci = driver.findArg(args, "pci=") orelse return false;
    const vendor = driver.findArg(args, "vendor=") orelse return false;
    const device = driver.findArg(args, "device=") orelse return false;
    const io = driver.findArg(args, "io=") orelse return false;
    const irq = driver.findArg(args, "irq=") orelse return false;

    // "BB:SS.FF" — the separators are checked so a malformed field cannot be
    // read as a valid address by accident.
    if (pci.len < 8 or pci[2] != ':' or pci[5] != '.') return false;
    const bus = driver.parseHex(pci, 2) orelse return false;
    const slot = driver.parseHex(pci[3..], 2) orelse return false;
    const function = driver.parseHex(pci[6..], 2) orelse return false;
    const vendor_id = driver.parseHex(vendor, 4) orelse return false;
    const device_id = driver.parseHex(device, 4) orelse return false;
    const io_base = driver.parseHex(io, 4) orelse return false;
    const irq_line = driver.parseHex(irq, 2) orelse return false;

    if (!isVirtioBlk(vendor_id, device_id) or io_base == 0) return false;

    g_dev.present = true;
    g_dev.bus = bus;
    g_dev.slot = slot;
    g_dev.function = function;
    g_dev.device_id = device_id;
    g_dev.irq = irq_line;
    g_dev.ports = .{ .base = @intCast(io_base) };
    return true;
}

// --- bring-up --------------------------------------------------------------

/// Take one kernel MSI vector for the completion interrupt and have `pci-bus`
/// program it into the device's MSI-X table.
///
/// The work is split three ways on purpose: the kernel owns the vector
/// namespace but never touches the device, `pci-bus` owns config space but
/// allocates no vectors, and this driver owns neither and only carries the
/// opaque address/data pair between them.
///
/// Returns true when the vector is bound. Every failure is non-fatal -- the
/// caller falls back to INTx.
fn setupMsix() bool {
    if (g_pci_endpoint < 0) return false;
    const reply_ep = driver.privateReplyEndpoint();
    if (reply_ep < 0) return false;

    const bdf: i32 = @intCast((g_dev.bus << 8) | (g_dev.slot << 3) | g_dev.function);
    const query = driver.call(g_pci_endpoint, reply_ep, op.PCI_IPC_MSI_QUERY, 1, bdf, 0, 0, 0) orelse return false;
    if (query.type != op.PCI_IPC_RESP or query.arg0 != PCI_MSI_KIND_MSIX or query.arg1 < 1) return false;

    // The vector is bound to the loop's endpoint, so a completion arrives as an
    // ordinary message the loop dispatches to onMessage.
    const desc = driver.msiAlloc(endpoint()) orelse return false;
    const entry: i32 = @intCast((@as(u32, @intCast(bdf)) << 8) | MSIX_ENTRY_QUEUE);
    const bind = driver.call(
        g_pci_endpoint,
        reply_ep,
        op.PCI_IPC_MSI_BIND,
        2,
        entry,
        @bitCast(desc.address_lo),
        @bitCast(desc.address_hi),
        @bitCast(desc.data),
    );
    if (bind == null or bind.?.type != op.PCI_IPC_RESP) {
        driver.msiFree(desc.vector);
        return false;
    }
    g_dev.msix_vector = desc.vector;
    g_dev.msix_enabled = true;
    return true;
}

/// Configure the request virtqueue over a pinned DMA region and program its
/// page frame number into the device. Returns the queue size, or null on
/// failure.
fn setupQueue() ?u16 {
    g_dev.ports.out16(REG_QUEUE_SELECT, REQUEST_QUEUE);
    const qsize = g_dev.ports.in16(REG_QUEUE_SIZE);
    if (qsize == 0 or qsize > MAX_QUEUE) return null;

    const queue = vring.Queue.create() orelse return null;
    const ring_bytes = vring.size(qsize, vring.LEGACY_ALIGN);
    const pages: u32 = @intCast((ring_bytes + 0xFFF) / 0x1000);
    const region = driver.regionAlloc(pages, driver.REGION_CACHE_WB) orelse return null;
    if (!queue.layout(region.base, region.phys, region.bytes, qsize, vring.LEGACY_ALIGN)) return null;
    queue.setNotify(notifyQueue, null);

    g_queue = queue;
    g_queue_ready = true;
    g_dev.ports.out32(REG_QUEUE_PFN, @intCast(region.phys >> 12));

    // Bind the request queue to its MSI-X entry. REG_QUEUE_SELECT still points
    // at this queue, and virtio reports a refusal through the readback rather
    // than any status bit, so the write is verified rather than assumed.
    if (g_dev.msix_enabled) {
        g_dev.ports.out16(REG_MSIX_QUEUE_VECTOR, MSIX_ENTRY_QUEUE);
        if (g_dev.ports.in16(REG_MSIX_QUEUE_VECTOR) == MSIX_NO_VECTOR) {
            driver.log("[virtio-blk] msix queue vector refused");
            return null;
        }
    }
    return qsize;
}

/// Bring the device up: reset, acknowledge, negotiate features, set the queue
/// up, read the capacity, and only then declare DRIVER_OK. Returns false with
/// the device left in the FAILED state when any step fails.
fn initializeDevice() bool {
    if (!g_dev.present or g_dev.ports.base == 0) return false;

    // Writing 0 resets the device; the bits then accumulate.
    setStatusBit(0);
    var state: u8 = STATUS_ACK;
    setStatusBit(state);
    state |= STATUS_DRIVER;
    setStatusBit(state);

    // Accept the read-only bit and FLUSH. Every other feature either changes a
    // layout this driver does not implement (BLK_SIZE) or is an optimisation it
    // does not use, and an unaccepted feature is simply one the device must not
    // rely on.
    //
    // FLUSH is accepted because a filesystem journal barrier needs it: ordering
    // a request after its reply only guarantees the device SAW the writes in
    // that order, and a volatile write cache can still lose the earlier ones.
    const device_features = g_dev.ports.in32(REG_DEVICE_FEATURES);
    g_dev.read_only = (device_features & FEATURE_RO) != 0;
    g_dev.flush_supported = (device_features & FEATURE_FLUSH) != 0;
    g_dev.ports.out32(REG_DRIVER_FEATURES, device_features & (FEATURE_RO | FEATURE_FLUSH));

    // Before queue setup: the queue's vector register only exists once MSI-X is
    // enabled, and setupQueue is what writes it.
    if (setupMsix()) {
        var line = driver.Line{};
        _ = line.str("[virtio-blk] msix enabled vector=").dec(g_dev.msix_vector);
        line.end();
    } else {
        driver.log("[virtio-blk] msix unavailable; falling back to intx");
    }

    const qsize = setupQueue() orelse {
        setStatusBit(state | STATUS_FAILED);
        return false;
    };

    // One page of pinned scratch, carved into MAX_INFLIGHT slots.
    g_scratch = driver.regionAlloc(1, driver.REGION_CACHE_WB) orelse {
        setStatusBit(state | STATUS_FAILED);
        return false;
    };

    const capacity = readCapacity();
    if (capacity > 0xFFFFFFFF) {
        g_dev.capacity_sectors = 0xFFFFFFFF;
        g_dev.capacity_truncated = true;
    } else {
        g_dev.capacity_sectors = @intCast(capacity);
    }

    state |= STATUS_DRIVER_OK;
    setStatusBit(state);
    g_dev.ready = true;

    var line = driver.Line{};
    _ = line.str("[virtio-blk] ready qsize=").dec(qsize);
    _ = line.str(" capacity=").dec(g_dev.capacity_sectors).str(" sectors");
    _ = line.str(" inflight=").dec(MAX_INFLIGHT);
    if (g_dev.read_only) _ = line.str(" read-only");
    if ((device_features & FEATURE_BLK_SIZE) != 0) {
        _ = line.str(" blk_size=").dec(g_dev.ports.in32(configBase() + CFG_BLK_SIZE));
    }
    line.end();
    if (g_dev.capacity_truncated) {
        driver.log("[virtio-blk] capacity exceeds 32 bits; reporting the addressable prefix");
    }
    return true;
}

/// Abandon the device after its queue state can no longer be trusted, and
/// refuse every later request with NOT_READY.
///
/// A published chain belongs to the DEVICE until it reports it on the used
/// ring, and there is no way to withdraw one. Freeing such a chain would hand
/// its descriptors to the next request while the device can still complete the
/// old one -- and because the data descriptor addresses the CALLER's block
/// buffer, the late write would land in another client's memory. So no chain is
/// freed here.
///
/// Resetting the device is what makes that impossible rather than merely
/// unlikely: writing 0 to the status register is the one operation that revokes
/// the queue, after which the device must not touch any of the memory it was
/// configured with. FAILED is written first because that is what the bit is
/// for. Every outstanding slot is then answered, so no client is left waiting.
fn quiesce(why: []const u8) void {
    g_dev.ready = false;
    g_queue_ready = false;
    setStatusBit(STATUS_FAILED);
    setStatusBit(0);
    for (&g_slots) |*slot| {
        if (slot.state == .in_flight or slot.state == .pending) {
            slot.result = status.WASMOS_ERR_VIRTIO_BLK_IO_ERROR;
            slot.state = .complete;
        }
    }
    var line = driver.Line{};
    _ = line.str("[virtio-blk] device abandoned: ").str(why);
    line.end();
}

// --- slots -----------------------------------------------------------------

fn slotHeader(index: usize) *ReqHeader {
    const off: u32 = @as(u32, @intCast(index)) * SLOT_STRIDE;
    return @ptrCast(@alignCast(g_scratch.base + off));
}

/// The device writes a slot's status byte, so it is reached through a volatile
/// view: between publish and completion it changes underneath this code.
fn slotStatus(index: usize) *volatile u8 {
    const off: u32 = @as(u32, @intCast(index)) * SLOT_STRIDE + SLOT_STATUS_OFF;
    return @ptrCast(g_scratch.base + off);
}

fn slotHeaderPhys(index: usize) u64 {
    return g_scratch.phys + @as(u64, @intCast(index)) * SLOT_STRIDE;
}

fn slotStatusPhys(index: usize) u64 {
    return g_scratch.phys + @as(u64, @intCast(index)) * SLOT_STRIDE + SLOT_STATUS_OFF;
}

fn claimSlot() ?usize {
    for (&g_slots, 0..) |*slot, i| {
        if (slot.state == .free) {
            slot.* = .{ .state = .pending };
            return i;
        }
    }
    return null;
}

/// The slot whose chain the device just reported, or null when the head names
/// no request this driver has outstanding.
fn slotForChain(head: u16) ?usize {
    for (&g_slots, 0..) |*slot, i| {
        if (slot.state == .in_flight and slot.chain_head == head) return i;
    }
    return null;
}

fn anyOutstanding() bool {
    for (&g_slots) |*slot| {
        if (slot.state != .free) return true;
    }
    return false;
}

// --- request handling ------------------------------------------------------

fn sendError(dest: i32, request_id: i32, code: i32) void {
    _ = driver.send(dest, endpoint(), op.BLOCK_IPC_ERROR, request_id, code, 0, 0, 0);
}

/// Validate the sector range every transfer request carries. Returns 0 when the
/// range is servable, or the packed code to refuse it with.
fn checkRange(lba: i32, count: i32) i32 {
    if (!g_dev.present or !g_dev.ready) return status.WASMOS_ERR_VIRTIO_BLK_NOT_READY;
    if (count <= 0 or count > @as(i32, @intCast(MAX_SECTORS)) or lba < 0) {
        return status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST;
    }
    // A request past the end of the device is a caller error, not something to
    // hand the device and let it refuse: answering here is what puts the reason
    // in the reply.
    const end: u64 = @as(u64, @intCast(lba)) + @as(u64, @intCast(count));
    if (end > g_dev.capacity_sectors) return status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST;
    return 0;
}

/// BLOCK_IPC_IDENTIFY_REQ: report the device geometry. arg1 is the sector count
/// and arg2 the unit, which is `blockUnit()` -- the SAME unit this driver
/// publishes and registers its `block` class instance under, as ATA also reports
/// the drive it identified. A filesystem driver resolves its mount point by
/// asking the device manager which rule covers the unit IDENTIFY named, so a
/// constant here sends it to whichever rule happens to cover unit 0 and its
/// mount never comes up. Answered inline because it needs no transfer.
fn handleIdentify(msg: *const co.IpcMessage) void {
    if (!g_dev.present or !g_dev.ready) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_NOT_READY);
        return;
    }
    _ = driver.send(
        msg.source,
        endpoint(),
        op.BLOCK_IPC_IDENTIFY_RESP,
        msg.request_id,
        0,
        @intCast(g_dev.capacity_sectors),
        @intCast(blockUnit()),
        0,
    );
}

/// BLOCK_IPC_FLUSH_REQ: commit the device's write cache, so a caller that orders
/// its writes can rely on that order surviving power loss.
///
/// A device that did not offer VIRTIO_BLK_F_FLUSH has no volatile write cache,
/// so the guarantee already holds and the reply is sent without touching the
/// queue. Answering success there is not a shortcut: there is nothing to commit.
fn acceptFlush(msg: *const co.IpcMessage) void {
    if (!g_dev.flush_supported) {
        _ = driver.send(msg.source, endpoint(), op.BLOCK_IPC_FLUSH_RESP, msg.request_id, 0, 0, 0, 0);
        return;
    }
    const index = claimSlot() orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_QUEUE_FULL);
        return;
    };
    const slot = &g_slots[index];
    slot.source = msg.source;
    slot.request_id = msg.request_id;
    slot.msg_type = msg.type;
    // A flush carries no data descriptor and no sector: the header's `sector`
    // field is reserved for it (virtio 1.2, 5.2.6).
    slot.lba = 0;
    slot.count = 0;
    slot.data_phys = 0;
    slot.borrow = 0;
    slot.borrow_bytes = 0;
    slot.result = 0;
    slot.idle_ticks = 0;
    slot.state = .pending;
    wakeRoot();
}

/// BLOCK_IPC_READ_REQ / BLOCK_IPC_WRITE_REQ: arg0 is the client's block buffer
/// named by physical address, arg1 the first sector, arg2 the sector count. The
/// buffer address goes straight into the data descriptor, so the device
/// transfers into (or out of) the client's pages directly.
fn acceptTransfer(msg: *const co.IpcMessage) void {
    const is_write = msg.type == op.BLOCK_IPC_WRITE_REQ;
    if (is_write and g_dev.read_only) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_READ_ONLY);
        return;
    }
    if (msg.arg0 <= 0) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST);
        return;
    }
    const check = checkRange(msg.arg1, msg.arg2);
    if (check != 0) {
        sendError(msg.source, msg.request_id, check);
        return;
    }
    const index = claimSlot() orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_QUEUE_FULL);
        return;
    };
    const slot = &g_slots[index];
    slot.source = msg.source;
    slot.request_id = msg.request_id;
    slot.msg_type = msg.type;
    slot.lba = @intCast(msg.arg1);
    slot.count = @intCast(msg.arg2);
    slot.data_phys = @intCast(msg.arg0);
}

/// BLOCK_IPC_READ_ZC_REQ: land whole sectors straight in the client's transfer
/// buffer. arg0 = buffer_id, arg1 = lba, arg2 = (borrow_id << 12) | count,
/// arg3 = destination byte offset.
///
/// The buffer is named twice because the two ways to reach it are addressed
/// differently, and this driver uses only the second: the packed borrow id names
/// the client's GRANT, and mapping it yields a device address the disk can be
/// pointed at. Without a borrow there is nothing to map -- this driver has no
/// staging path, so the request is refused and the client falls back to
/// BLOCK_IPC_READ_REQ.
fn acceptReadZeroCopy(msg: *const co.IpcMessage) void {
    const count = msg.arg2 & ZC_COUNT_MASK;
    const borrow = @as(i32, @intCast(@as(u32, @bitCast(msg.arg2)) >> ZC_BORROW_SHIFT));
    if (msg.arg0 <= 0 or msg.arg3 < 0 or borrow <= 0) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST);
        return;
    }
    const check = checkRange(msg.arg1, count);
    if (check != 0) {
        sendError(msg.source, msg.request_id, check);
        return;
    }
    const index = claimSlot() orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_QUEUE_FULL);
        return;
    };

    const bytes = @as(u32, @intCast(count)) * SECTOR_BYTES;
    const data_phys = driver.dmaMapBorrow(borrow, @intCast(msg.arg3), bytes, driver.DMA_DIR_FROM_DEVICE) orelse {
        g_slots[index].state = .free;
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST);
        return;
    };

    const slot = &g_slots[index];
    slot.source = msg.source;
    slot.request_id = msg.request_id;
    slot.msg_type = msg.type;
    slot.lba = @intCast(msg.arg1);
    slot.count = @intCast(count);
    slot.data_phys = data_phys;
    slot.borrow = borrow;
    slot.borrow_bytes = bytes;
}

/// Service a completion interrupt: ack it where one is owed, then reap every
/// chain the device has reported.
///
/// Both halves of an INTx ack matter. Reading the virtio ISR de-asserts the
/// level-triggered line at the DEVICE, and irqAck unmasks it at the kernel,
/// which keeps a dispatched line masked until every driver sharing it has
/// acked. An MSI vector is edge-triggered and exclusively owned, so neither
/// applies to it.
fn serviceCompletions() void {
    if (g_dev.irq_routed) {
        // Reading ISR is itself the de-assertion; the value is not wanted.
        _ = g_dev.ports.in8(REG_ISR_STATUS);
        driver.irqAck(g_dev.irq);
    }
    if (!g_queue_ready) return;

    while (g_queue.getUsed(null)) |head| {
        const index = slotForChain(head) orelse {
            // The device reported a chain this driver has no record of, so the
            // ring is out of step and no descriptor can be trusted -- recycling
            // one the device still owns corrupts a client's buffer.
            quiesce("used ring reported a chain that was not in flight");
            return;
        };
        // The device has given the chain back, so its descriptors and the
        // buffers they name are the driver's again.
        g_queue.freeChain(head);
        const slot = &g_slots[index];
        slot.result = if (slotStatus(index).* == REQ_STATUS_OK)
            0
        else
            status.WASMOS_ERR_VIRTIO_BLK_IO_ERROR;
        slot.state = .complete;
    }
}

/// Wake the root task, if it is parked. Handlers run on the runner's stack
/// rather than in a coroutine, so this is how work reaches the task.
fn wakeRoot() void {
    if (!g_root_parked) return;
    g_root_parked = false;
    _ = g_wake_promise.resolve(0);
}

/// Every message the loop could not match to an in-flight request id. Runs from
/// `poll`, so it does the cheap part -- accept or refuse a request, reap
/// completions -- and leaves the virtqueue work to the root task it wakes.
fn onMessage(user: ?*anyopaque, msg: *const co.IpcMessage) callconv(.c) void {
    _ = user;
    switch (msg.type) {
        op.BLOCK_IPC_IDENTIFY_REQ => handleIdentify(msg),
        op.BLOCK_IPC_READ_REQ, op.BLOCK_IPC_WRITE_REQ => acceptTransfer(msg),
        op.BLOCK_IPC_FLUSH_REQ => acceptFlush(msg),
        op.BLOCK_IPC_READ_ZC_REQ => acceptReadZeroCopy(msg),
        IPC_IRQ_EVENT_TYPE, IPC_MSI_EVENT_TYPE => serviceCompletions(),
        else => {
            // A message with no reply address is a stray notification, not a
            // request to refuse.
            if (msg.source >= 0) {
                sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_UNSUPPORTED_REQUEST);
            }
            return;
        },
    }
    wakeRoot();
}

/// An idle interval elapsed with nothing delivered. Ages every outstanding
/// request and wakes the root task, which is what enforces the deadline.
fn onTimeout(user: ?*anyopaque) callconv(.c) void {
    _ = user;
    for (&g_slots) |*slot| {
        if (slot.state == .in_flight or slot.state == .pending) slot.idle_ticks += 1;
    }
    wakeRoot();
}

// --- root task -------------------------------------------------------------

/// Put one pending slot's chain on the queue. Returns false when the ring has no
/// room, which is ordinary backpressure: the slot stays pending and is retried
/// after a completion frees descriptors.
fn submit(index: usize) bool {
    const slot = &g_slots[index];
    const is_flush = slot.msg_type == op.BLOCK_IPC_FLUSH_REQ;
    const is_write = slot.msg_type == op.BLOCK_IPC_WRITE_REQ;
    const req_type: u32 = if (is_flush) REQ_TYPE_FLUSH else if (is_write) REQ_TYPE_OUT else REQ_TYPE_IN;

    slotHeader(index).* = .{ .type = req_type, .ioprio = 0, .sector = slot.lba };
    slotStatus(index).* = REQ_STATUS_UNSET;

    const bufs = [3]vring.Buf{
        .{ .addr = slotHeaderPhys(index), .len = @sizeOf(ReqHeader), .flags = 0 },
        .{
            .addr = slot.data_phys,
            .len = slot.count * SECTOR_BYTES,
            // The direction of this one descriptor is the whole difference
            // between a read and a write.
            .flags = if (req_type == REQ_TYPE_IN) vring.DESC_F_WRITE else 0,
        },
        .{ .addr = slotStatusPhys(index), .len = 1, .flags = vring.DESC_F_WRITE },
    };

    // A flush has no data: header and status only. Passing the middle descriptor
    // with a zero length would put a zero-length buffer on the queue, which the
    // device is not required to accept.
    const head = (if (is_flush)
        g_queue.allocChain(&[2]vring.Buf{ bufs[0], bufs[2] })
    else
        g_queue.allocChain(&bufs)) orelse return false;
    slot.chain_head = head;
    slot.state = .in_flight;
    g_queue.publish(head);
    return true;
}

/// Answer one finished slot and release it.
fn complete(index: usize) void {
    const slot = &g_slots[index];
    if (slot.borrow > 0) {
        // Sync before unmapping and on the failure path too: the device may have
        // written part of the range before erroring out. The offset is relative
        // to the MAPPING, which already starts at the destination offset, so
        // passing that again would run off the end.
        driver.dmaSyncBorrow(slot.borrow, 0, slot.borrow_bytes, driver.DMA_SYNC_FROM_DEVICE);
        driver.dmaUnmapBorrow(slot.borrow);
    }
    if (slot.result != 0) {
        sendError(slot.source, slot.request_id, slot.result);
    } else {
        _ = driver.send(
            slot.source,
            endpoint(),
            switch (slot.msg_type) {
                op.BLOCK_IPC_WRITE_REQ => op.BLOCK_IPC_WRITE_RESP,
                op.BLOCK_IPC_FLUSH_REQ => op.BLOCK_IPC_FLUSH_RESP,
                else => op.BLOCK_IPC_READ_RESP,
            },
            slot.request_id,
            0,
            @intCast(slot.count),
            0,
            0,
        );
    }
    slot.state = .free;
}

/// The driver's only coroutine: answer finished requests, put pending ones on
/// the queue, enforce the deadline, and park when there is nothing to do.
///
/// Stackless, so it keeps no state across a yield beyond the slot table -- it
/// re-enters at the top every time and re-derives what to do from the slots.
/// That is why there is no step counter: the slot states ARE the program
/// counter, and they are the same state the handlers manipulate.
///
/// It never returns `complete`: a driver has no finishing condition, and the
/// runner pumps only while the root task is alive.
fn rootTask(user: ?*anyopaque, out_value: *usize) callconv(.c) i32 {
    _ = user;
    _ = out_value;

    var progressed = false;
    var published = false;
    var expired = false;

    for (&g_slots, 0..) |*slot, i| {
        switch (slot.state) {
            .complete => {
                complete(i);
                progressed = true;
            },
            .pending => {
                if (slot.idle_ticks >= MAX_IDLE_TICKS) {
                    expired = true;
                } else if (g_queue_ready and submit(i)) {
                    published = true;
                    progressed = true;
                }
            },
            .in_flight => {
                if (slot.idle_ticks >= MAX_IDLE_TICKS) expired = true;
            },
            .free => {},
        }
    }
    // One doorbell for the whole batch: publishing several chains and ringing
    // once is the point of separating publish from kick.
    if (published) g_queue.kick();

    // A request the device never reported past its deadline means the driver has
    // lost track of a chain the device may still be working on, so the queue
    // cannot be reused -- see quiesce.
    if (expired) {
        quiesce("a request went unreported past its deadline");
        return co.TaskResult.yielded;
    }

    // The timer exists only to age outstanding requests; with none, park
    // indefinitely rather than wake for nothing.
    armIdleTimer(anyOutstanding());

    // Re-enter rather than park while that pass changed something: a completion
    // may have freed descriptors a pending slot was waiting for.
    if (progressed) return co.TaskResult.yielded;

    // Nothing to do. Park on a future the handlers settle, so the runner polls --
    // and its poll parks on the loop's select set, which is what makes an idle
    // driver sleep instead of spin.
    g_wake_future.init(&g_wake_promise);
    g_root_parked = true;
    switch (g_wake_future.awaitValue()) {
        .pending => return co.TaskResult.yielded,
        else => {
            // The future settled before the await, or the runtime refused it.
            // Either way nothing is parked, so loop rather than claim to be.
            g_root_parked = false;
            return co.TaskResult.yielded;
        },
    }
}

// --- entry -----------------------------------------------------------------

/// Bring the driver up before the coroutine runtime starts.
///
/// The runner has already created the endpoint and initialised the event loop,
/// and calls this once; the runtime does not exist yet, so everything here is
/// necessarily synchronous. That is the contract, not a shortcut -- there is no
/// future to await on before there is a runtime to await in.
///
/// A failure leaves the device not-ready. The root task then still runs and
/// answers every request with NOT_READY, which is what a client needs: a driver
/// that exited would leave its class instance registered to a dead process.
/// Announce this disk to the device-manager inventory, which is the registry a
/// block rule matches against -- so without this the disk exists as a service
/// but no filesystem can be mounted on it.
///
/// arg0 = unit (backend-local), arg1 = sectors, arg2 bit0 = present, and arg3
/// names the backend so the device manager can tell this disk apart from an ATA
/// unit with the same number.
///
/// Fire-and-forget, matching the ATA publisher: the inventory is a notification
/// and the device manager owns what it does with it. A device manager that is
/// not up yet simply never learns about this disk, which is why the lookup is
/// retried rather than assumed.
fn publishBlockDevice(proc_endpoint: i32) void {
    if (!g_dev.ready) return;
    const devmgr = driver.lookupService(proc_endpoint, "devmgr.inv", 32, 256) orelse {
        driver.log("[virtio-blk] devmgr inventory unavailable; disk not published");
        return;
    };
    _ = driver.send(
        devmgr,
        endpoint(),
        op.DEVMGR_PUBLISH_BLOCK_DEVICE,
        0,
        @intCast(blockUnit()),
        @intCast(g_dev.capacity_sectors),
        1, // present; active_service is the device manager's to set
        abi.BLOCK_BACKEND_VIRTIO_BLK,
    );
    var line = driver.Line{};
    _ = line.str("[virtio-blk] published unit=").dec(blockUnit());
    _ = line.str(" instance=").dec(blockClassInstance()).str(" sectors=").dec(g_dev.capacity_sectors);
    line.end();
}

fn prepare(user: ?*anyopaque, arg0: i32, arg1: i32, arg2: i32, arg3: i32) callconv(.c) void {
    _ = user;
    _ = arg0;
    _ = arg1;
    _ = arg2;
    _ = arg3;

    loop().default_on_message = @ptrCast(&onMessage);
    loop().on_timeout = @ptrCast(&onTimeout);
    armIdleTimer(false);

    const proc_endpoint = driver.procEndpoint();
    if (proc_endpoint < 0 or endpoint() < 0) {
        driver.log("[virtio-blk] no process-manager endpoint");
        return;
    }

    // Resolved before device bring-up, because that is where the MSI-X vector is
    // bound. Its absence is not fatal: bring-up falls back to INTx.
    g_pci_endpoint = driver.lookupService(proc_endpoint, "pci", 16, 1024) orelse blk: {
        driver.log("[virtio-blk] pci service unavailable; msi-x disabled");
        break :blk -1;
    };

    if (!probeFromStartupArgs()) {
        driver.log("[virtio-blk] startup args name no virtio-blk device");
        return;
    }
    var probe = driver.Line{};
    _ = probe.str("[virtio-blk] probe ok bus=").dec(g_dev.bus).str(" slot=").dec(g_dev.slot);
    _ = probe.str(" dev=0x").hex(g_dev.device_id, 4).str(" io=0x").hex(g_dev.ports.base, 4);
    probe.end();

    if (!initializeDevice()) {
        driver.log("[virtio-blk] device init failed");
        return;
    }

    // INTx fallback only. With MSI-X bound the device's INTx is disabled, so
    // routing the shared line would merely subscribe this driver to other
    // devices' interrupts, which is exactly what MSI-X exists to avoid.
    //
    // Without it, routing is what keeps the line from re-firing forever: the
    // device asserts on every completion and nothing else clears it. The line
    // is routed to the loop's endpoint so an interrupt arrives as a message the
    // loop dispatches, like everything else this driver waits on.
    if (!g_dev.msix_enabled and g_dev.irq < 16) {
        if (driver.irqRoute(g_dev.irq, endpoint())) {
            g_dev.irq_routed = true;
            var line = driver.Line{};
            _ = line.str("[virtio-blk] irq routed line=").dec(g_dev.irq);
            line.end();
        } else {
            // No vector and no line: completions have no signal at all, and the
            // idle timer is the only thing that will move a request -- straight
            // to its deadline. Say so rather than appear healthy.
            driver.log("[virtio-blk] irq route failed; completions have no signal");
        }
    }

    // The service is registered ON the loop's endpoint, so client requests,
    // interrupts and replies all arrive at the one place the loop drains.
    //
    // The concrete name is "virtio-blk"; the class is what a client looks up, so
    // it finds whichever block backend is present without naming this driver.
    // The plain "block" NAME is deliberately not claimed: the ATA driver holds
    // it for the boot disk.
    if (driver.registerService(proc_endpoint, endpoint(), "virtio-blk", "block", blockClassInstance(), 1) == null) {
        driver.log("[virtio-blk] service registration failed");
        return;
    }
    publishBlockDevice(proc_endpoint);
    driver.notifyReady(proc_endpoint);
}
