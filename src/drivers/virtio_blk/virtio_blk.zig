//! virtio_blk.zig — VirtIO block-device driver, in Zig.
//!
//! Serves the block-device IPC interface (BLOCK_IPC_IDENTIFY_REQ,
//! BLOCK_IPC_READ_REQ, BLOCK_IPC_WRITE_REQ, BLOCK_IPC_READ_ZC_REQ) on top of a
//! legacy virtio-blk PCI device, so a filesystem driver reads a virtio disk
//! through exactly the protocol it reads an ATA disk through.
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
//! mapped for the device instead. Only the header and the status byte live in
//! this driver's own pinned region, and together they are 17 bytes.
//!
//! Requests are strictly serialised: one chain is in flight at a time, and the
//! driver blocks on the device's completion interrupt in between. That costs
//! queue depth and is the obvious thing to lift later; it also means every
//! completion the used ring reports belongs to the request being waited on.
//!
//! A chain the device has not reported still BELONGS to it, and virtio offers no
//! way to withdraw one. A timeout is therefore terminal rather than retryable:
//! the driver abandons the device (`quiesce`) instead of recycling descriptors
//! the device may still complete into a client's buffer.
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

const driver = @import("driver.zig");
const vring = @import("vring.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");

/// `WASMOS_PCI_MSI_KIND_MSIX`, as PCI_IPC_MSI_QUERY reports it in arg0. MSI-X
/// wins when a device offers both: it addresses each vector independently,
/// where plain MSI needs one aligned block of consecutive vectors.
const PCI_MSI_KIND_MSIX: i32 = 2;

/// 0x1AF4 is the Red Hat / virtio vendor id. Both device ids below identify a
/// block device and are accepted interchangeably: 0x1001 is the transitional
/// (legacy) id, 0x1042 is 0x1040 + virtio device type 2.
const PCI_VENDOR_VIRTIO: u32 = 0x1AF4;
const PCI_DEVICE_BLK_LEGACY: u32 = 0x1001;
const PCI_DEVICE_BLK_MODERN: u32 = 0x1042;

/// Legacy virtqueue registers, as offsets into the device's I/O window. MSI-X
/// is not enabled by this driver, so the device-specific configuration stays at
/// 0x14; enabling it would insert two vector registers there and shift the
/// configuration to 0x18.
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

/// Request types, and the status byte the device writes back.
const REQ_TYPE_IN: u32 = 0; // device -> memory (a disk read)
const REQ_TYPE_OUT: u32 = 1; // memory -> device (a disk write)
const REQ_TYPE_FLUSH: u32 = 4;
const REQ_STATUS_OK: u8 = 0;
/// Sentinel written into the status byte before a request is published. The
/// device overwrites it, so seeing it after a completion means the device
/// reported the chain without touching its status -- a device bug, reported as
/// an I/O error rather than mistaken for success.
const REQ_STATUS_UNSET: u8 = 0xFF;

/// virtio-blk defines exactly one queue, index 0, the requestq. MAX_QUEUE caps
/// the queue size this driver accepts from the device; the ring must fit the
/// region allocated for it.
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

/// Offsets of the header and the status byte within the driver's scratch
/// region. They are separate descriptors, so they only need to be distinct;
/// keeping the status well clear of the header keeps a device that overruns the
/// header from landing on it.
const SCRATCH_HEADER_OFF: u32 = 0;
const SCRATCH_STATUS_OFF: u32 = 64;

/// Completion is interrupt-driven: the device raises its INTx line when it has
/// used a buffer and the driver blocks on the routed event. The interval and
/// try count are a safety net for a lost or unroutable interrupt, not the
/// mechanism -- polling a device interrupt on a timer leaves a shared line
/// asserted between ticks, which livelocks a single-CPU guest.
const IRQ_WAIT_MS: i32 = 50;
const IRQ_MAX_WAITS: u32 = 40; // ~2s ceiling before a request is declared timed out

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
};

var g_dev: Device = .{};
var g_endpoint: i32 = -1;
var g_select: i32 = -1;
/// Interrupts get their own endpoint and select set so the completion wait can
/// drain them without consuming (and discarding) pending block requests.
var g_irq_endpoint: i32 = -1;
var g_irq_select: i32 = -1;
var g_irq_routed: bool = false;
/// `pci-bus` owns PCI config space and is the only party that can program this
/// device's MSI-X table; its absence just means the INTx fallback.
var g_pci_endpoint: i32 = -1;

var g_queue: vring.Queue = undefined;
var g_queue_ready: bool = false;
/// Pinned region holding the request header and the status byte. Everything
/// else a request touches is the client's memory.
var g_scratch: driver.Region = undefined;

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

/// True while completions arrive as IPC on the interrupt endpoint, by either
/// mechanism -- which is what decides whether the completion wait parks on that
/// endpoint or on the service one.
fn irqActive() bool {
    return g_irq_routed or g_dev.msix_enabled;
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
    if (g_pci_endpoint < 0 or g_irq_endpoint < 0) return false;
    const reply_ep = driver.privateReplyEndpoint();
    if (reply_ep < 0) return false;

    const bdf: i32 = @intCast((g_dev.bus << 8) | (g_dev.slot << 3) | g_dev.function);
    const query = driver.call(g_pci_endpoint, reply_ep, op.PCI_IPC_MSI_QUERY, 1, bdf, 0, 0, 0) orelse return false;
    if (query.type != op.PCI_IPC_RESP or query.arg0 != PCI_MSI_KIND_MSIX or query.arg1 < 1) return false;

    const desc = driver.msiAlloc(g_irq_endpoint) orelse return false;
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

    // Accept nothing but the read-only bit. Every other feature either changes
    // a layout this driver does not implement (BLK_SIZE) or is an optimisation
    // it does not use, and an unaccepted feature is simply one the device must
    // not rely on.
    const device_features = g_dev.ports.in32(REG_DEVICE_FEATURES);
    g_dev.read_only = (device_features & FEATURE_RO) != 0;
    g_dev.ports.out32(REG_DRIVER_FEATURES, device_features & FEATURE_RO);

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

    // One page of pinned scratch for the request header and status byte.
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

// --- interrupts ------------------------------------------------------------

/// Service the device's interrupt. Both halves matter: reading the virtio ISR
/// de-asserts the level-triggered INTx line at the DEVICE, and irq_ack unmasks
/// it at the kernel, which keeps a dispatched line masked until every driver
/// sharing it has acked. A PCI INTx line is shared, so an unserviced assertion
/// here re-fires for every sharer on each unmask.
fn serviceIrq() void {
    if (!irqActive()) return;
    // Several events may have queued; the payload only names the source and the
    // work is the same for each.
    while (driver.drain(g_irq_endpoint)) {}
    if (g_dev.msix_enabled) {
        // Nothing to de-assert and nothing to ack: the vector is edge-triggered
        // and exclusive to this device, so none of the ceremony below applies.
        return;
    }
    // Reading ISR is itself the de-assertion; the value is not wanted.
    _ = g_dev.ports.in8(REG_ISR_STATUS);
    driver.irqAck(g_dev.irq);
}

// --- requests --------------------------------------------------------------

/// Build, publish and await one virtio-blk request. `data_phys` is the device
/// address of the caller's data buffer -- the client's block buffer or a mapped
/// borrow, never this driver's memory. Returns 0, or a packed
/// WASMOS_ERR_VIRTIO_BLK_* code.
fn submit(req_type: u32, sector: u64, data_phys: u64, data_len: u32) i32 {
    if (!g_dev.ready or !g_queue_ready) return status.WASMOS_ERR_VIRTIO_BLK_NOT_READY;

    const header: *ReqHeader = @ptrCast(@alignCast(g_scratch.base + SCRATCH_HEADER_OFF));
    header.* = .{ .type = req_type, .ioprio = 0, .sector = sector };

    // The device writes the status byte, so it is read and written through a
    // volatile view: between publish and the completion it changes underneath
    // this code.
    const status_byte: *volatile u8 = @ptrCast(g_scratch.base + SCRATCH_STATUS_OFF);
    status_byte.* = REQ_STATUS_UNSET;

    const bufs = [3]vring.Buf{
        .{ .addr = g_scratch.phys + SCRATCH_HEADER_OFF, .len = @sizeOf(ReqHeader), .flags = 0 },
        .{
            .addr = data_phys,
            .len = data_len,
            // The direction of this one descriptor is the whole difference
            // between a read and a write.
            .flags = if (req_type == REQ_TYPE_IN) vring.DESC_F_WRITE else 0,
        },
        .{ .addr = g_scratch.phys + SCRATCH_STATUS_OFF, .len = 1, .flags = vring.DESC_F_WRITE },
    };

    const head = g_queue.allocChain(&bufs) orelse return status.WASMOS_ERR_VIRTIO_BLK_QUEUE_FULL;
    g_queue.publish(head);
    g_queue.kick();

    var waits: u32 = 0;
    while (waits <= IRQ_MAX_WAITS) : (waits += 1) {
        if (g_queue.getUsed(null)) |completed| {
            // Requests are serialised, so the only chain the device can be
            // reporting is this one. Anything else means the ring is out of
            // step, and neither chain can be accounted for -- reading a status
            // byte that may belong to a different request, or recycling
            // descriptors the device still owns, both corrupt a client's
            // buffer. Abandon the device instead.
            if (completed != head) {
                quiesce("used ring reported a chain that was not in flight");
                return status.WASMOS_ERR_VIRTIO_BLK_IO_ERROR;
            }
            // Now the device has given the chain back, so its descriptors and
            // the buffers they name are the driver's again.
            g_queue.freeChain(head);
            return if (status_byte.* == REQ_STATUS_OK) 0 else status.WASMOS_ERR_VIRTIO_BLK_IO_ERROR;
        }
        if (waits == IRQ_MAX_WAITS) break;
        // Block until the completion interrupt arrives, or the safety-net
        // interval elapses. Only IRQ events land on this endpoint, so draining
        // it cannot swallow a block request. A wait that FAILS returns
        // immediately, so looping on it would spin rather than block: give up
        // on the completion and let the timeout path run.
        const parked = if (irqActive())
            driver.selectWait(g_irq_select, IRQ_WAIT_MS)
        else
            driver.selectWait(g_select, IRQ_WAIT_MS);
        if (parked == .failed) break;
        serviceIrq();
    }

    quiesce("a request went unreported past its deadline");
    return status.WASMOS_ERR_VIRTIO_BLK_TIMEOUT;
}

/// Abandon the device after its queue state can no longer be trusted, and
/// refuse every later request with NOT_READY.
///
/// A published chain belongs to the DEVICE until it reports it on the used
/// ring, and there is no way to withdraw one: a timeout means the driver has
/// lost track of a chain the device may still be working on. Freeing that chain
/// would hand its descriptors to the next request while the device can still
/// complete the old one -- and because the data descriptor addresses the
/// CALLER's block buffer, the late write would land in another client's memory.
/// So the chain is deliberately NOT freed.
///
/// Resetting the device is what actually makes that impossible rather than
/// merely unlikely: writing 0 to the status register is the one operation that
/// revokes the queue, after which the device must not touch any of the memory
/// it was configured with. FAILED is written first because that is what the bit
/// is for -- it tells the device the driver gave up, and it is visible to
/// anything inspecting the device afterwards.
///
/// Recovery would mean re-running bring-up from probe. That is deliberately not
/// attempted: a virtio-blk request that misses a 2-second deadline means
/// something is wrong that a silent retry would only hide.
fn quiesce(why: []const u8) void {
    g_dev.ready = false;
    g_queue_ready = false;
    setStatusBit(STATUS_FAILED);
    setStatusBit(0);
    // Service the line before returning: leaving it asserted turns a lost
    // completion into an unbounded interrupt storm.
    serviceIrq();
    var line = driver.Line{};
    _ = line.str("[virtio-blk] device abandoned: ").str(why);
    line.end();
}

fn sendError(dest: i32, request_id: i32, code: i32) void {
    _ = driver.send(dest, g_endpoint, op.BLOCK_IPC_ERROR, request_id, code, 0, 0, 0);
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

/// BLOCK_IPC_IDENTIFY_REQ: report the device geometry. arg1 is the sector
/// count and arg2 the unit index, which is always 0 -- a virtio-blk device is
/// one disk, and a second disk is a second device with its own driver instance.
fn handleIdentify(source: i32, request_id: i32) void {
    if (!g_dev.present or !g_dev.ready) {
        sendError(source, request_id, status.WASMOS_ERR_VIRTIO_BLK_NOT_READY);
        return;
    }
    _ = driver.send(
        source,
        g_endpoint,
        op.BLOCK_IPC_IDENTIFY_RESP,
        request_id,
        0,
        @intCast(g_dev.capacity_sectors),
        0,
        0,
    );
}

/// BLOCK_IPC_READ_REQ / BLOCK_IPC_WRITE_REQ: arg0 is the client's block buffer
/// named by physical address, arg1 the first sector, arg2 the sector count. The
/// buffer address goes straight into the data descriptor, so the device
/// transfers into (or out of) the client's pages directly.
fn handleTransfer(msg_type: i32, source: i32, request_id: i32, buffer_phys: i32, lba: i32, count: i32) void {
    const is_write = msg_type == op.BLOCK_IPC_WRITE_REQ;
    if (is_write and g_dev.read_only) {
        sendError(source, request_id, status.WASMOS_ERR_VIRTIO_BLK_READ_ONLY);
        return;
    }
    if (buffer_phys <= 0) {
        sendError(source, request_id, status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST);
        return;
    }
    const check = checkRange(lba, count);
    if (check != 0) {
        sendError(source, request_id, check);
        return;
    }

    const rc = submit(
        if (is_write) REQ_TYPE_OUT else REQ_TYPE_IN,
        @intCast(lba),
        @intCast(buffer_phys),
        @as(u32, @intCast(count)) * SECTOR_BYTES,
    );
    if (rc != 0) {
        sendError(source, request_id, rc);
        return;
    }
    _ = driver.send(
        source,
        g_endpoint,
        if (is_write) op.BLOCK_IPC_WRITE_RESP else op.BLOCK_IPC_READ_RESP,
        request_id,
        0,
        count,
        0,
        0,
    );
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
fn handleReadZeroCopy(source: i32, request_id: i32, buffer_id: i32, lba: i32, packed_count: i32, dst_offset: i32) void {
    const count = packed_count & ZC_COUNT_MASK;
    const borrow = @as(i32, @intCast(@as(u32, @bitCast(packed_count)) >> ZC_BORROW_SHIFT));
    if (buffer_id <= 0 or dst_offset < 0 or borrow <= 0) {
        sendError(source, request_id, status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST);
        return;
    }
    const check = checkRange(lba, count);
    if (check != 0) {
        sendError(source, request_id, check);
        return;
    }

    const bytes = @as(u32, @intCast(count)) * SECTOR_BYTES;
    const data_phys = driver.dmaMapBorrow(borrow, @intCast(dst_offset), bytes, driver.DMA_DIR_FROM_DEVICE) orelse {
        sendError(source, request_id, status.WASMOS_ERR_VIRTIO_BLK_BAD_REQUEST);
        return;
    };

    const rc = submit(REQ_TYPE_IN, @intCast(lba), data_phys, bytes);
    // Sync before unmapping and on the failure path too: the device may have
    // written part of the range before erroring out. The offset is relative to
    // the MAPPING, which already starts at dst_offset, so passing it again
    // would run off the end.
    driver.dmaSyncBorrow(borrow, 0, bytes, driver.DMA_SYNC_FROM_DEVICE);
    driver.dmaUnmapBorrow(borrow);

    if (rc != 0) {
        sendError(source, request_id, rc);
        return;
    }
    _ = driver.send(source, g_endpoint, op.BLOCK_IPC_READ_RESP, request_id, 0, count, 0, 0);
}

fn dispatch(msg: driver.Message) void {
    switch (msg.type) {
        op.BLOCK_IPC_IDENTIFY_REQ => handleIdentify(msg.source, msg.request_id),
        op.BLOCK_IPC_READ_REQ, op.BLOCK_IPC_WRITE_REQ => handleTransfer(
            msg.type,
            msg.source,
            msg.request_id,
            msg.arg0,
            msg.arg1,
            msg.arg2,
        ),
        op.BLOCK_IPC_READ_ZC_REQ => handleReadZeroCopy(
            msg.source,
            msg.request_id,
            msg.arg0,
            msg.arg1,
            msg.arg2,
            msg.arg3,
        ),
        else => sendError(msg.source, msg.request_id, status.WASMOS_ERR_VIRTIO_BLK_UNSUPPORTED_REQUEST),
    }
}

// --- entry -----------------------------------------------------------------

/// Driver entry point: bring the virtio-blk device up, register under the
/// "block" class, and serve BLOCK_IPC_* requests forever.
///
/// On success this does not return. A bring-up failure does return, with a
/// packed WASMOS_ERR_DRIVER_* code: there is nothing to serve without a device,
/// and a driver that loops anyway would answer every request with the same
/// refusal while holding the class instance.
pub export fn initialize() callconv(.c) i32 {
    const proc_endpoint = driver.procEndpoint();
    if (proc_endpoint < 0) return status.WASMOS_ERR_DRIVER_NO_PROC_ENDPOINT;

    g_endpoint = driver.createEndpoint();
    if (g_endpoint < 0) return status.WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    g_select = driver.selectCreate();
    if (g_select < 0 or !driver.selectAdd(g_select, g_endpoint)) {
        driver.log("[virtio-blk] select setup failed");
        return status.WASMOS_ERR_DRIVER_SELECT_SETUP;
    }

    // Interrupts need their own endpoint, and it must exist before the device
    // is brought up so the line can be routed as soon as the device is live.
    g_irq_endpoint = driver.createEndpoint();
    g_irq_select = if (g_irq_endpoint >= 0) driver.selectCreate() else -1;
    if (g_irq_endpoint < 0 or g_irq_select < 0 or
        !driver.selectAdd(g_irq_select, g_irq_endpoint) or
        !driver.selectAdd(g_select, g_irq_endpoint))
    {
        driver.log("[virtio-blk] irq endpoint setup failed; using timed waits");
        g_irq_endpoint = -1;
        g_irq_select = -1;
    }

    // Resolved before device bring-up, because that is where the MSI-X vector is
    // bound. Its absence is not fatal: bring-up falls back to INTx.
    g_pci_endpoint = driver.lookupService(proc_endpoint, "pci", 16, 1024) orelse blk: {
        driver.log("[virtio-blk] pci service unavailable; msi-x disabled");
        break :blk -1;
    };

    if (!probeFromStartupArgs()) {
        driver.log("[virtio-blk] startup args name no virtio-blk device");
        return status.WASMOS_ERR_DRIVER_NO_DEVICE_IDENTITY;
    }
    var probe = driver.Line{};
    _ = probe.str("[virtio-blk] probe ok bus=").dec(g_dev.bus).str(" slot=").dec(g_dev.slot);
    _ = probe.str(" dev=0x").hex(g_dev.device_id, 4).str(" io=0x").hex(g_dev.ports.base, 4);
    probe.end();

    if (!initializeDevice()) {
        driver.log("[virtio-blk] device init failed");
        return status.WASMOS_ERR_DRIVER_DEVICE_INIT;
    }

    // INTx fallback only. With MSI-X bound the device's INTx is disabled, so
    // routing the shared line would merely subscribe this driver to other
    // devices' interrupts, which is exactly what MSI-X exists to avoid.
    //
    // Without it, routing is what keeps the line from re-firing forever: the
    // device asserts on every completion and nothing else clears it. Failure is
    // not fatal -- the completion wait falls back to its timed safety net.
    if (!g_dev.msix_enabled and g_dev.irq < 16 and g_irq_endpoint >= 0) {
        if (driver.irqRoute(g_dev.irq, g_irq_endpoint)) {
            g_irq_routed = true;
            var line = driver.Line{};
            _ = line.str("[virtio-blk] irq routed line=").dec(g_dev.irq);
            line.end();
        } else {
            driver.log("[virtio-blk] irq route failed; using timed waits");
        }
    }

    // The concrete name is "virtio-blk"; the class is what a client looks up, so
    // it finds whichever block backend is present without naming this driver.
    // The plain "block" NAME is deliberately not claimed: the ATA driver holds
    // it for the boot disk.
    if (driver.registerService(proc_endpoint, g_endpoint, "virtio-blk", "block", 0, 1) == null) {
        driver.log("[virtio-blk] service registration failed");
        return status.WASMOS_ERR_DRIVER_REGISTER;
    }
    driver.notifyReady(proc_endpoint);

    while (true) {
        // Ack the line whenever an event has arrived, not only while a request
        // is waiting: a PCI INTx line is shared and the kernel keeps a
        // dispatched line masked until EVERY sharer acks, so deferring this
        // stalls the other drivers' interrupts too.
        serviceIrq();
        while (driver.drain(g_endpoint)) {
            const msg = driver.lastMessage();
            // A stray notification or an IRQ echo carries no reply address.
            if (msg.source < 0) continue;
            dispatch(msg);
        }
        if (driver.selectWait(g_select, 1000) == .failed) {
            // A failed wait returns immediately, so this loop would stop
            // blocking. Yield explicitly rather than tightening into a hot loop.
            // FIXME: with a dead select set there is nothing left to park on;
            // the real fix is not to lose the set.
            driver.yield();
        }
    }
}
