/* ata.c - ATA/IDE block device WASM driver.
 * Serves the early storage bootstrap path: identify plus small read/write
 * requests over the block-device IPC interface (BLOCK_IPC_READ_REQ /
 * BLOCK_IPC_WRITE_REQ), which is what the FAT driver needs to mount the ESP and
 * to service its overwrite-only write path.  Transfers run as bus-master DMA
 * where the controller and the request allow it and as PIO otherwise.
 * Runs inside the WASM runtime; all I/O port accesses go through capability-
 * checked host-call imports. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/* The driver names no absolute port. Its spawn profile grants I/O windows in the
 * order its manifest declares them; region 0 is the task-file window, which on a
 * legacy IDE controller spans the fixed ISA ports [0x1F0, 0x3F7] and therefore
 * covers the device-control register at 0x3F6 as well. Offsets below are
 * relative to that window's base, so where firmware actually put it -- or
 * whether it moves -- is the kernel's problem, not this file's. */
/* Region 0 is the PRIMARY task file, region 1 the SECONDARY: two windows, in
 * the order the manifest declares them. Which one a register access lands in
 * follows from the unit being addressed, so it is selected once per operation
 * (ata_select_channel) rather than passed through every helper — the driver
 * serves one request at a time, so there is one live channel at a time. */
#define ATA_REG_CTRL 0x206u /* 0x3F6 - 0x1F0: device control / alternate status */

/* ATA task-file registers (ATA/ATAPI-6, "Register Delivery"), as offsets from
 * the region base above. Three of them are two registers sharing one address,
 * distinguished by direction: 0x01 reads Error and writes Features, 0x07 reads
 * Status and writes Command. Reading Status has the side effect of clearing a
 * pending interrupt, which is why the polled paths read the ALTERNATE status at
 * ATA_REG_CTRL when they only want to look.
 *
 * LBA0..LBA2 carry bits 0-7, 8-15 and 16-23 of the sector address; the top four
 * bits go in the low nibble of HDDEVSEL, which also selects master/slave. That
 * 28-bit total is the limit of the LBA28 addressing this driver uses. */
#define ATA_REG_DATA 0x00
#define ATA_REG_ERROR 0x01
#define ATA_REG_FEATURES 0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0 0x03
#define ATA_REG_LBA1 0x04
#define ATA_REG_LBA2 0x05
#define ATA_REG_HDDEVSEL 0x06
#define ATA_REG_COMMAND 0x07
#define ATA_REG_STATUS 0x07

/* Bus-master IDE. Region 1 is the window the manifest declares as BAR 4, which
 * device-manager resolves and grants; the driver never learns the address.
 * Registers are per-channel, primary first (PIIX datasheet 5.2). */
/* Region 2 because the two task-file windows come first; declaration order in
 * linker.metadata IS the region index. Both channels share this one window: the
 * secondary channel's registers sit at +8 (PIIX datasheet 5.2), which
 * ata_bm_offset adds. */
#define ATA_BM_REGION 2u
#define ATA_BM_COMMAND 0x00u
#define ATA_BM_STATUS 0x02u
#define ATA_BM_PRDT 0x04u

#define ATA_BM_CMD_START (1u << 0)
/* Direction is named from the DEVICE's point of view: set for a disk read,
 * where the controller writes into memory. */
#define ATA_BM_CMD_TO_MEMORY (1u << 3)
#define ATA_BM_STATUS_ACTIVE (1u << 0)
#define ATA_BM_STATUS_ERROR (1u << 1)
#define ATA_BM_STATUS_IRQ (1u << 2)

/* A PRD entry is a physical run; 0 in the count field means 64 KiB. An entry may
 * not cross a 64 KiB boundary, so one buffer can need two. */
#define ATA_PRD_EOT 0x8000u
#define ATA_PRD_MAX 2u
#define ATA_PRD_BOUNDARY 0x10000u

/* One Physical Region Descriptor, in the layout the bus-master controller reads
 * directly from memory: a 32-bit PHYSICAL address (hence the 4 GiB ceiling on
 * where a DMA buffer may live), a byte count where 0 encodes 64 KiB, and a flags
 * word whose only defined bit is ATA_PRD_EOT marking the last entry. */
typedef struct __attribute__((packed)) {
    uint32_t base;
    uint16_t bytes;
    uint16_t flags;
} ata_prd_t;

/* Command-register opcodes (ATA/ATAPI-6, LBA28 forms). IDENTIFY returns one
 * 512-byte block of device parameters through PIO regardless of DMA support, so
 * it is the probe that establishes whether a unit is present at all.
 * READ_SECTORS/WRITE_SECTORS are the PIO transfers; READ_DMA is the bus-master
 * read the driver prefers when the controller and request allow it (there is no
 * WRITE_DMA here -- writes always take the PIO path). CACHE_FLUSH forces the
 * device to commit its write cache, and is what makes a completed write durable
 * rather than merely accepted. */
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

/* Status-register bits. BSY means the device owns the task file and every other
 * bit in the register is meaningless until it clears, so it must be tested
 * first. DRQ means a data block is ready to move. ERR means the command failed
 * and the Error register holds the reason. A status byte of 0xFF is not a status
 * at all -- it is the float an absent device reads back. */
#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

/* Device Control (0x3F6). nIEN set = the drive never asserts INTRQ, so the
 * transfer paths can only poll; it is cleared once the IRQ line is routed and
 * set again whenever the driver falls back to polling. */
#define ATA_CTRL_NIEN (1u << 1)

/* Primary channel legacy line. The PIIX IDE function reports no PCI interrupt
 * pin (config 0x3D = 0, irq_hint 0xFF), so the line is not discoverable from the
 * device record — it is the fixed ISA assignment for the primary channel, and
 * the spawn profile grants exactly 14|15 for this driver. */
#define ATA_IRQ_LINE 14u

/* ATA_SECTOR_SIZE is the fixed 512-byte block this driver assumes throughout.
 * ATA_MAX_READ_SECTORS bounds one read request and must not exceed
 * WASMOS_BLOCK_ZC_MAX_SECTORS, since that is what a zero-copy client may ask
 * for. ATA_UNIT_COUNT is the two devices a single IDE channel addresses (master
 * and slave) across both channels, which is a property of the bus, not a local
 * budget.
 * ATA_CLIENT_MAP_CAP sizes the source-to-unit binding table. It is deliberately
 * larger than ATA_UNIT_COUNT and is not what limits clients: a unit is claimed
 * EXCLUSIVELY by the first source bound to it, so a second client asking for an
 * already-claimed unit is refused while the table still has free slots. */
#define ATA_SECTOR_SIZE 512u
#define ATA_MAX_READ_SECTORS 8u
/* Four: two channels of master and slave. A unit's channel is unit >> 1 and its
 * drive-select bit is unit & 1. */
#define ATA_UNIT_COUNT 4u
#define ATA_CHANNEL_COUNT 2u
#define ATA_CLIENT_MAP_CAP 8u

/* Wait budgets. The polled bound counts I/O-delay iterations; the interrupt
 * bound is much smaller because each attempt sleeps rather than spinning
 * (200 x 10 ms = a ~2 s ceiling before a transfer is declared failed). */
#define ATA_POLL_ATTEMPTS 100000u
#define ATA_IRQ_ATTEMPTS 200u
#define ATA_IRQ_WAIT_MS 10
/* Consecutive sleeps that produce no interrupt before the driver stops trusting
 * one. Without this, a line that is routed but silently undelivered would cost
 * the full timeout on every sector for the rest of the boot. */
#define ATA_IRQ_PROBE_LIMIT 8u

/* The channel every register access below addresses: 0 = primary, 1 =
 * secondary. Set by ata_select_channel before any unit-scoped operation. */
static uint32_t g_channel;

static int32_t g_block_endpoint = -1;
static int32_t g_devmgr_endpoint = -1;
/* Kept because the class registration happens during unit probing, well after
 * initialize() has the endpoint on its stack. */
static int32_t g_proc_endpoint_cached = -1;
static uint32_t g_sector_count = 0;
static uint8_t g_present = 0;
static uint32_t g_unit_sectors[ATA_UNIT_COUNT];
static uint8_t g_unit_present[ATA_UNIT_COUNT];
static uint8_t g_sector_buf[ATA_SECTOR_SIZE];
static uint8_t g_dma_read_ok_logged = 0;
static uint8_t g_dma_write_ok_logged = 0;
static uint8_t g_zc_logged = 0;
static uint8_t g_zc_dma_logged = 0;
static int32_t g_client_owner[ATA_CLIENT_MAP_CAP];
static uint8_t g_client_unit[ATA_CLIENT_MAP_CAP];
/* 0 while the entry is only a SELECTION (the unit this client named in
 * IDENTIFY), 1 once a transfer has claimed the unit exclusively. Selections are
 * not exclusive: several clients may name the same unit, and the first transfer
 * decides who gets it. */
static uint8_t g_client_claimed[ATA_CLIENT_MAP_CAP];

/* PRD table. It lives in this process's own block buffer, which is already
 * everything the controller needs -- contiguous, pinned, page-aligned and below
 * 4 GiB -- and which this driver otherwise never uses, since it writes into the
 * CALLER's buffer. region_alloc would be the obvious choice but needs megabytes
 * of linear-memory headroom to map a window into, and a bootstrap driver has no
 * business carrying that for 16 bytes of descriptor.
 * g_dma_ready gates the whole path: without a bus-master window or a PRD table
 * the driver simply stays on PIO. */
static int32_t g_prd_phys = -1;
static uint8_t g_dma_ready;
static uint8_t g_dma_logged;

/* Interrupt state. Events land on their own endpoint so draining them cannot
 * discard a queued block request. g_irq_active means both halves are live: the
 * line is routed AND nIEN is clear at the drive. */
static int32_t g_irq_endpoint = -1;
static int32_t g_irq_select = -1;
static uint8_t g_irq_active;
static uint8_t g_irq_seen;       /* at least one interrupt has actually arrived */
static uint32_t g_irq_dry_waits; /* consecutive sleeps that produced nothing */

/* A refused read cannot be reported through the value (0xFF is a real status),
 * so a failure reads as 0 -- which has neither BSY nor DRQ set and so cannot be
 * mistaken for a ready drive by any caller below. */
/* The I/O window the live channel's task file sits in. */
static uint32_t ata_region(void) {
    return g_channel;
}

/* Point every register access at `unit`'s channel. Called at the top of each
 * unit-scoped operation; a path that touched registers without it would drive
 * the wrong channel, so there is exactly one place per operation to check. */
static void ata_select_channel(uint8_t unit) {
    g_channel = (uint32_t)(unit >> 1) & 1u;
}

/* Bus-master registers are per-channel within the one window: the secondary
 * channel's copy sits 8 bytes above the primary's (PIIX datasheet 5.2). */
static uint32_t ata_bm_offset(uint32_t offset) {
    return offset + (g_channel * 8u);
}

static uint8_t ata_read_status(void) {
    uint32_t value = 0;
    if (wasmos_io_region_in8(ata_region(), ATA_REG_STATUS, &value) != 0) {
        return 0u;
    }
    return (uint8_t)(value & 0xFFu);
}

static uint16_t ata_read_data16(void) {
    uint32_t value = 0;
    if (wasmos_io_region_in16(ata_region(), ATA_REG_DATA, &value) != 0) {
        return 0u;
    }
    return (uint16_t)(value & 0xFFFFu);
}

/* Consume any pending interrupt events. Two things are owed and both matter:
 * reading the status register is what de-asserts the drive's INTRQ, and irq_ack
 * is what reopens the line the kernel masked on dispatch. An event that arrives
 * and is never acked leaves the line masked permanently — i.e. the disk dead.
 * Returns non-zero if an event was consumed. */
static int ata_service_irq(void) {
    int drained = 0;
    if (g_irq_endpoint < 0) {
        return 0;
    }
    while (wasmos_ipc_drain(g_irq_endpoint) > 0) {
        drained = 1;
    }
    if (drained) {
        (void)ata_read_status();
        (void)wasmos_irq_ack((int32_t)ATA_IRQ_LINE);
        if (!g_irq_seen) {
            g_irq_seen = 1;
            (void)printf("[ata] interrupt-driven transfers active\n");
        }
        g_irq_dry_waits = 0;
    }
    return drained;
}

/* Stop using the interrupt and go back to polling, leaving a clean state: quiet
 * the drive first, settle anything owed, then drop the route so the line is
 * masked rather than left asserted with nobody listening. */
static void ata_disable_interrupts(const char* why) {
    if (!g_irq_active) {
        return;
    }
    g_irq_active = 0;
    /* nIEN on the PRIMARY: that is the only channel whose interrupt was ever
     * enabled, and this may run while a secondary transfer is the live one. */
    g_channel = 0u;
    wasmos_io_region_out8(ata_region(), ATA_REG_CTRL, ATA_CTRL_NIEN);
    (void)ata_service_irq();
    (void)wasmos_irq_unroute((int32_t)ATA_IRQ_LINE);
    (void)printf("[ata] %s; falling back to polled transfers\n", why);
}

/* One wait step between status reads. With the interrupt live this blocks, so
 * waiting for a sector costs no CPU; otherwise it is a short I/O delay. A
 * routed-but-undelivered interrupt is detected here and abandoned once, rather
 * than being paid for on every sector. */
static void ata_wait_step(void) {
    /* The interrupt is routed for the PRIMARY channel only, so a transfer on the
     * secondary polls. Routing the secondary's line 15 as well would mean
     * telling two lines apart from one event and acking the right one, and an
     * event acked on the wrong line leaves that line masked — the disk dead.
     * The secondary carries no boot-critical volume, so it pays a poll instead. */
    if (!g_irq_active || g_channel != 0u) {
        wasmos_io_wait();
        return;
    }
    if (!wasmos_sys_wait_parked(wasmos_ipc_select_wait_timeout(g_irq_select, ATA_IRQ_WAIT_MS))) {
        /* The wait itself failed, so it returned without blocking. Continuing to
         * call it turns this into a spin; drop to the timed I/O delay instead. */
        ata_disable_interrupts("interrupt wait failed");
        wasmos_io_wait();
        return;
    }
    if (ata_service_irq() || g_irq_seen) {
        return;
    }
    if (++g_irq_dry_waits >= ATA_IRQ_PROBE_LIMIT) {
        ata_disable_interrupts("no interrupt from the drive");
    }
}

static uint32_t ata_wait_attempts(void) {
    return g_irq_active ? ATA_IRQ_ATTEMPTS : ATA_POLL_ATTEMPTS;
}

/* Status is read BEFORE waiting in both loops below: the condition is often
 * already true (a pre-command idle check never has an interrupt coming), and
 * sleeping first would trade a spin for a guaranteed timeout. */
static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < ata_wait_attempts(); ++i) {
        if ((ata_read_status() & ATA_SR_BSY) == 0) {
            return 0;
        }
        ata_wait_step();
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < ata_wait_attempts(); ++i) {
        uint8_t status = ata_read_status();
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ)) {
            return 0;
        }
        ata_wait_step();
    }
    return -1;
}

static int ata_identify_unit(uint8_t unit, uint16_t* out_words) {
    if (!out_words) {
        return -1;
    }
    ata_select_channel(unit);
    wasmos_io_region_out8(ata_region(), ATA_REG_HDDEVSEL, (uint8_t)(0xA0u | ((unit & 1u) << 4)));
    wasmos_io_wait();
    wasmos_io_region_out8(ata_region(), ATA_REG_SECCOUNT0, 0);
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA0, 0);
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA1, 0);
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA2, 0);
    wasmos_io_region_out8(ata_region(), ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (ata_read_status() == 0) {
        return -1;
    }
    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < 256; ++i) {
        out_words[i] = ata_read_data16();
    }
    return 0;
}

/* Claim one `block` class instance for a present disk.
 *
 * A class instance is a DISK, not a driver, so this controller registers once
 * per present unit -- the class registry admits several instances from one
 * owner, and all of them name this same endpoint because a client picks the
 * disk with the unit argument the block protocol already carries.
 *
 * The instance is (backend << 8) | unit rather than a number handed out on
 * registration: it is derived from what the disk IS, so it is the same every
 * boot regardless of which driver probed first, and a client can decode it back
 * into the pair a device-manager rule names.
 *
 * This is the ONLY way a client finds this driver: no plain "block" name is
 * registered, because a name resolves to one provider system-wide and would
 * make this controller the only disk anybody could reach. */
static void ata_register_block_class(uint8_t unit, uint8_t present) {
    if (!present || g_proc_endpoint_cached < 0) {
        return;
    }
    uint32_t instance = ((uint32_t)BLOCK_BACKEND_ATA << 8) | (uint32_t)unit;
    if (wasmos_svc_register_class(g_proc_endpoint_cached,
                                  g_block_endpoint,
                                  "ata",
                                  "block",
                                  instance,
                                  64 + (int32_t)unit) < 0) {
        (void)printf("[ata] block class register failed unit=%u\n", (unsigned)unit);
        return;
    }
    (void)printf("[ata] block class unit=%u instance=%u\n", (unsigned)unit, (unsigned)instance);
}

static void ata_publish_block_device(uint8_t unit, uint32_t sectors, uint8_t present) {
    if (g_devmgr_endpoint < 0 || g_block_endpoint < 0) {
        return;
    }
    uint32_t flags = 0;
    if (present) {
        flags |= 1u;
    }
    if (unit == 0 && g_present) {
        flags |= 2u;
    }
    /* arg3 names the backend. The unit alone does not identify a disk: it is
     * backend-local, so this driver's unit 0 and a virtio-blk device's unit 0
     * are different disks, and the device manager keys on the pair. */
    (void)wasmos_ipc_send(g_devmgr_endpoint,
                          g_block_endpoint,
                          DEVMGR_PUBLISH_BLOCK_DEVICE,
                          0,
                          (int32_t)unit,
                          (int32_t)sectors,
                          (int32_t)flags,
                          BLOCK_BACKEND_ATA);
}

/* Where a read deposits each sector. The block buffer is the caller's own
 * staging area addressed by physical address; the transfer buffer belongs to the
 * original client and reaches this driver as a reborrow, so the kernel admits
 * the write on the strength of that grant. Only the destination differs — the
 * sector loop is identical. */
typedef struct {
    uint8_t to_xfer;     /* 0 = block buffer (phys), 1 = client transfer buffer */
    int32_t id;          /* buffer_phys, or the transfer buffer's object id */
    uint32_t dst_offset; /* byte offset of sector 0 within the destination */
} ata_sink_t;

static int ata_sink_write(const ata_sink_t* sink, const uint8_t* src, uint32_t len,
                          uint32_t sector_offset) {
    uint32_t offset = sink->dst_offset + sector_offset;
    if (sink->to_xfer) {
        return wasmos_xfer_buffer_write(sink->id, src, (int32_t)len, (int32_t)offset);
    }
    return wasmos_block_buffer_write(
        sink->id, addr_cast(int32_t, src), (int32_t)len, (int32_t)offset);
}

/* Bus-master register helpers. Region-addressed like everything else, so a
 * failure here means the window was never granted, not that the address is
 * wrong. */
static uint8_t ata_bm_read8(uint32_t offset) {
    uint32_t value = 0;
    if (wasmos_io_region_in8(ATA_BM_REGION, ata_bm_offset(offset), &value) != 0) {
        return 0xFFu;
    }
    return (uint8_t)(value & 0xFFu);
}

static int ata_bm_write8(uint32_t offset, uint8_t value) {
    return wasmos_io_region_out8(ATA_BM_REGION, ata_bm_offset(offset), value);
}

/* Describe `bytes` at `phys` as PRD entries, splitting at the 64 KiB boundary a
 * single entry may not cross. Returns the entry count, or 0 if it would not fit
 * (which sends the caller back to PIO rather than programming a bad table). */
static uint32_t ata_build_prd(uint64_t phys, uint32_t bytes) {
    ata_prd_t prd[ATA_PRD_MAX];
    uint32_t used = 0;
    while (bytes > 0u && used < ATA_PRD_MAX) {
        uint64_t next_boundary = (phys + ATA_PRD_BOUNDARY) & ~((uint64_t)ATA_PRD_BOUNDARY - 1u);
        uint32_t run = (uint32_t)(next_boundary - phys);
        if (run > bytes) {
            run = bytes;
        }
        prd[used].base = (uint32_t)phys;
        prd[used].bytes = (uint16_t)(run & 0xFFFFu); /* 0 encodes a full 64 KiB */
        prd[used].flags = 0;
        phys += run;
        bytes -= run;
        used++;
    }
    /* `used == 0` means a zero-byte request: the loop never ran, so there is no
     * last entry to mark and `used - 1` would index off the front of the array.
     * The callers reject a zero sector count, so this guard is what keeps that
     * an invariant of this function rather than an assumption about them. */
    if (bytes > 0u || used == 0u) {
        return 0;
    }
    prd[used - 1u].flags = ATA_PRD_EOT;
    if (wasmos_block_buffer_write(
            g_prd_phys, addr_cast(int32_t, prd), (int32_t)(used * sizeof(ata_prd_t)), 0) != 0) {
        return 0;
    }
    return used;
}

/* Read whole sectors straight into physical memory, no CPU copy at all. The
 * controller writes the destination itself; the driver only issues the command
 * and waits for the interrupt it already routes. Returns 0, or -1 to fall back
 * to PIO -- every failure here is recoverable that way. */
static int ata_read_lba28_dma(uint8_t unit, uint32_t lba, uint8_t count, uint64_t dest_phys) {
    ata_select_channel(unit);
    uint8_t bm_status = 0;

    if (!g_dma_ready || count == 0 || dest_phys == 0 || (dest_phys >> 32) != 0) {
        return -1;
    }
    if (ata_build_prd(dest_phys, (uint32_t)count * ATA_SECTOR_SIZE) == 0) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    /* Stop any previous transfer, point the controller at the table, and set the
     * direction before arming anything. */
    (void)ata_bm_write8(ATA_BM_COMMAND, 0);
    if (wasmos_io_region_out32(ATA_BM_REGION, ata_bm_offset(ATA_BM_PRDT), g_prd_phys) != 0) {
        return -1;
    }
    (void)ata_bm_write8(ATA_BM_COMMAND, ATA_BM_CMD_TO_MEMORY);
    /* Error and interrupt are write-1-to-clear; clear stale ones so the status
     * read after completion describes this transfer. */
    (void)ata_bm_write8(ATA_BM_STATUS, ATA_BM_STATUS_ERROR | ATA_BM_STATUS_IRQ);

    wasmos_io_region_out8(ata_region(),
                          ATA_REG_HDDEVSEL,
                          (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_region_out8(ata_region(), ATA_REG_SECCOUNT0, count);
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_COMMAND, ATA_CMD_READ_DMA);

    /* Arm the controller only after the drive has the command. */
    (void)ata_bm_write8(ATA_BM_COMMAND, ATA_BM_CMD_TO_MEMORY | ATA_BM_CMD_START);

    for (uint32_t i = 0; i < ata_wait_attempts(); ++i) {
        bm_status = ata_bm_read8(ATA_BM_STATUS);
        if ((bm_status & ATA_BM_STATUS_IRQ) != 0 || (bm_status & ATA_BM_STATUS_ERROR) != 0) {
            break;
        }
        if ((bm_status & ATA_BM_STATUS_ACTIVE) == 0) {
            break; /* controller went idle: either done or never started */
        }
        ata_wait_step();
    }

    (void)ata_bm_write8(ATA_BM_COMMAND, 0);
    (void)ata_bm_write8(ATA_BM_STATUS, ATA_BM_STATUS_ERROR | ATA_BM_STATUS_IRQ);

    if ((bm_status & ATA_BM_STATUS_ERROR) != 0 || (bm_status & ATA_BM_STATUS_IRQ) == 0) {
        return -1;
    }
    if ((ata_read_status() & ATA_SR_ERR) != 0) {
        return -1;
    }
    if (!g_dma_logged) {
        g_dma_logged = 1;
        (void)printf("[ata] bus-master DMA active\n");
    }
    return 0;
}

/* The whole point of the zero-copy path: map the client's own pages for the
 * controller and let it write them directly, so the sectors are never touched by
 * the CPU at all. Without this the "zero-copy" read still costs two copies (port
 * reads into a staging sector, then a copy into the transfer buffer) and only
 * saves the hop through the block buffer.
 *
 * The borrow is what makes this admissible: it is the client's grant, so mapping
 * it cannot reach memory the client did not offer, and the kernel bounds the
 * range. Returns 0, or -1 to fall back to the copying path -- an old client that
 * sends no borrow, a controller without bus-master, and a mapping refusal all
 * land here, which is why the copy path has to stay. */
static int ata_read_zc_dma(uint8_t unit, uint32_t lba, uint8_t count, int32_t borrow_id,
                           uint32_t dst_offset) {
    uint32_t bytes = (uint32_t)count * ATA_SECTOR_SIZE;
    int32_t dest_phys = 0;
    int rc = 0;

    if (!g_dma_ready || borrow_id <= 0 || count == 0) {
        return -1;
    }
    dest_phys = wasmos_dma_map_borrow(
        borrow_id, (int32_t)dst_offset, (int32_t)bytes, WASMOS_DMA_DIR_FROM_DEVICE);
    if (dest_phys <= 0) {
        return -1; /* negative is a packed WASMOS_ERR_DMA_* code; either way, copy instead */
    }

    rc = ata_read_lba28_dma(unit, lba, count, (uint64_t)(uint32_t)dest_phys);
    /* Sync before unmap and on the failure path too: the controller may have
     * written part of the range before erroring out. The offset is relative to
     * the MAPPING, not to the buffer -- the mapping already starts at
     * dst_offset, so passing it again would run off the end. */
    (void)wasmos_dma_sync_borrow(borrow_id, 0, (int32_t)bytes, WASMOS_DMA_SYNC_FROM_DEVICE);
    (void)wasmos_dma_unmap_borrow(borrow_id);
    if (rc == 0 && !g_zc_dma_logged) {
        g_zc_dma_logged = 1;
        (void)printf("[ata] zero-copy reads: direct DMA into client buffer\n");
    }
    return rc;
}

static int ata_read_lba28(uint8_t unit, uint32_t lba, uint8_t count, const ata_sink_t* sink) {
    ata_select_channel(unit);
    if (count == 0 || count > ATA_MAX_READ_SECTORS || !sink || sink->id <= 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_region_out8(ata_region(),
                          ATA_REG_HDDEVSEL,
                          (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_region_out8(ata_region(), ATA_REG_SECCOUNT0, count);
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    /* Reads are staged through a local 512-byte sector buffer and then handed to
     * the sink, which is either the caller's block buffer or the original
     * client's transfer buffer. */
    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        uint16_t* out = (uint16_t*)g_sector_buf;
        for (uint32_t i = 0; i < 256; ++i) {
            out[i] = ata_read_data16();
        }
        if (ata_sink_write(sink, g_sector_buf, ATA_SECTOR_SIZE, sector * ATA_SECTOR_SIZE) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ata_write_lba28(uint8_t unit, uint32_t lba, uint8_t count, uint32_t buffer_phys) {
    ata_select_channel(unit);
    if (count == 0 || count > ATA_MAX_READ_SECTORS || buffer_phys == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_region_out8(ata_region(),
                          ATA_REG_HDDEVSEL,
                          (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_region_out8(ata_region(), ATA_REG_SECCOUNT0, count);
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_region_out8(ata_region(), ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        if (wasmos_block_buffer_copy((int32_t)buffer_phys,
                                     addr_cast(int32_t, g_sector_buf),
                                     ATA_SECTOR_SIZE,
                                     (int32_t)(sector * ATA_SECTOR_SIZE)) != 0) {
            return -1;
        }
        uint16_t* in = (uint16_t*)g_sector_buf;
        for (uint32_t i = 0; i < 256; ++i) {
            wasmos_io_region_out16(ata_region(), ATA_REG_DATA, in[i]);
        }
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    wasmos_io_region_out8(ata_region(), ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}

/* Commit the drive's write cache (BLOCK_IPC_FLUSH_REQ). The same command every
 * write already ends with, issued on its own so a caller can order writes across
 * requests -- which is what a journal barrier needs and what request ordering
 * alone does not give. */
static int ata_flush_unit(uint8_t unit) {
    ata_select_channel(unit);
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    /* CACHE FLUSH acts on whichever drive the channel currently addresses, so
     * the drive-select bit is written first: a flush issued without it would
     * commit the OTHER drive's cache and report success for this one. The
     * command takes no LBA, so the address bits are zero. */
    wasmos_io_region_out8(ata_region(), ATA_REG_HDDEVSEL, (uint8_t)(0xE0u | ((unit & 1u) << 4)));
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    wasmos_io_region_out8(ata_region(), ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}

static void ata_send_resp(int32_t reply_ep, int32_t req_id, int32_t type, int32_t status,
                          int32_t arg1) {
    wasmos_ipc_send(reply_ep, g_block_endpoint, type, req_id, status, arg1, 0, 0);
}

static void ata_log(const char* s) {
    if (!s) {
        return;
    }
    (void)printf("%s", s);
}

static void ata_log_dma_active(uint8_t is_write) {
    if (is_write) {
        if (!g_dma_write_ok_logged) {
            g_dma_write_ok_logged = 1;
            ata_log("[ata] dma write path active\n");
        }
    } else {
        if (!g_dma_read_ok_logged) {
            g_dma_read_ok_logged = 1;
            ata_log("[ata] dma read path active\n");
        }
    }
}

/* Bring up bus-master DMA: a pinned page for the PRD table, and a probe of the
 * bus-master status register to confirm the window was actually granted. Both
 * are optional -- without them the driver stays on PIO, which is why every
 * failure here just leaves g_dma_ready clear. */
static void ata_dma_setup(void) {
    uint32_t probe = 0;

    if (!g_present) {
        return;
    }
    if (wasmos_io_region_in8(ATA_BM_REGION, ata_bm_offset(ATA_BM_STATUS), &probe) != 0) {
        (void)printf("[ata] no bus-master window; PIO only\n");
        return;
    }
    g_prd_phys = wasmos_block_buffer_phys();
    if (g_prd_phys <= 0) {
        (void)printf("[ata] no prd table backing; PIO only\n");
        return;
    }
    g_dma_ready = 1u;
}

/* Record which unit a client means, without claiming it.
 *
 * IDENTIFY is how a client says which of this controller's drives it is talking
 * about, and a transfer must then go to THAT drive. Without this the transfer
 * path would fall back to "the first unclaimed unit", so a client that
 * identified drive 1 could read drive 0 whenever drive 0 happened to be free --
 * silently the wrong disk, and dependent on which client bound first.
 *
 * A selection is not exclusive, so identifying a drive someone else is using is
 * allowed; the claim happens on the first transfer and is refused there. An
 * existing CLAIMED entry is left alone: a client cannot re-aim itself at another
 * drive once its transfers have started. */
static void ata_select_unit_for_source(int32_t source, uint8_t unit) {
    if (source < 0 || unit >= ATA_UNIT_COUNT) {
        return;
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        if (g_client_owner[i] == source) {
            if (!g_client_claimed[i]) {
                g_client_unit[i] = unit;
            }
            return;
        }
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        if (g_client_owner[i] < 0) {
            g_client_owner[i] = source;
            g_client_unit[i] = unit;
            g_client_claimed[i] = 0;
            return;
        }
    }
}

/* Reported once at startup rather than per request. A per-request "dma fallback"
 * line would read like an intermittent runtime failure; the fallback is neither
 * intermittent nor a failure — see ata_dma_prepare. */
static void ata_log_transfer_mode(void) {
    (void)printf("[ata] transfers: %s\n",
                 g_dma_ready ? "bus-master DMA into physical or borrowed destinations, else PIO"
                             : "PIO only");
}

/* TODO(zero-copy writes): the read direction maps the client's borrow and lets
 * the controller write those pages directly (ata_read_zc_dma); the write
 * direction still has no equivalent, because there is no zero-copy write opcode
 * for a client to hand its borrow down with. BLOCK_IPC_WRITE_REQ names the
 * driver's own block buffer, so these hooks have nothing to map and deny
 * unconditionally, which sends the transfer through that buffer. Giving writes
 * the same treatment means a WRITE_ZC opcode carrying (buffer_id, borrow_id)
 * and a WASMOS_DMA_DIR_TO_DEVICE mapping. */
static int ata_dma_prepare(int32_t source_endpoint, uint32_t offset, uint32_t length,
                           uint32_t direction_flags, int32_t* out_device_addr) {
    (void)source_endpoint;
    (void)offset;
    (void)length;
    (void)direction_flags;
    (void)out_device_addr;
    return WASMOS_ERR_DMA_DENY; /* force PIO fallback */
}

static int ata_dma_finish(int32_t source_endpoint, uint32_t offset, uint32_t length,
                          uint32_t direction_flags) {
    /* Unreachable while ata_dma_prepare always denies (callers guard on OK). */
    (void)source_endpoint;
    (void)offset;
    (void)length;
    (void)direction_flags;
    return 0;
}

static int ata_assign_unit_for_source(int32_t source, int32_t preferred_unit, uint8_t* out_unit) {
    if (!out_unit || source < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        if (g_client_owner[i] != source) {
            continue;
        }
        if (g_client_claimed[i]) {
            *out_unit = g_client_unit[i];
            return 0;
        }
        /* A selection recorded by IDENTIFY. Turn it into the claim, unless
         * another client got that drive first. */
        for (uint32_t j = 0; j < ATA_CLIENT_MAP_CAP; ++j) {
            if (j != i && g_client_owner[j] >= 0 && g_client_claimed[j] &&
                g_client_unit[j] == g_client_unit[i]) {
                return -1;
            }
        }
        if (!g_unit_present[g_client_unit[i]]) {
            return -1;
        }
        g_client_claimed[i] = 1;
        *out_unit = g_client_unit[i];
        return 0;
    }
    if (preferred_unit >= 0 && preferred_unit < (int32_t)ATA_UNIT_COUNT) {
        uint8_t unit = (uint8_t)preferred_unit;
        uint8_t claimed = 0;
        if (!g_unit_present[unit]) {
            return -1;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] >= 0 && g_client_claimed[i] && g_client_unit[i] == unit) {
                claimed = 1;
                break;
            }
        }
        if (claimed) {
            return -1;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] < 0) {
                g_client_owner[i] = source;
                g_client_unit[i] = unit;
                g_client_claimed[i] = 1;
                *out_unit = unit;
                return 0;
            }
        }
        return -1;
    }
    for (uint32_t unit = 0; unit < ATA_UNIT_COUNT; ++unit) {
        uint8_t claimed = 0;
        if (!g_unit_present[unit]) {
            continue;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] >= 0 && g_client_claimed[i] && g_client_unit[i] == unit) {
                claimed = 1;
                break;
            }
        }
        if (claimed) {
            continue;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] < 0) {
                g_client_owner[i] = source;
                g_client_unit[i] = (uint8_t)unit;
                g_client_claimed[i] = 1;
                *out_unit = (uint8_t)unit;
                return 0;
            }
        }
    }
    return -1;
}

static int ata_handle_ipc(int32_t type, int32_t source, int32_t req_id, int32_t arg0, int32_t arg1,
                          int32_t arg2, int32_t arg3) {
    uint8_t unit = 0;
    if (!g_present) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_NOT_READY, 0);
        return 0;
    }

    /* IDENTIFY is answered WITHOUT claiming the unit. Exclusivity exists to keep
     * two filesystems off one disk, and reading a disk's geometry does neither;
     * requiring a claim made a mounted disk unqueryable, which defeats
     * discovering it by class in the first place. Only the transfer paths below
     * bind a client. */
    if (type == BLOCK_IPC_IDENTIFY_REQ) {
        uint8_t want = (arg0 >= 0 && arg0 < (int32_t)ATA_UNIT_COUNT) ? (uint8_t)arg0 : 0u;
        if (!g_unit_present[want]) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT, 0);
            return 0;
        }
        /* Remember which drive this client means, so its transfers reach that
         * one rather than whichever happens to be free. */
        ata_select_unit_for_source(source, want);
        wasmos_ipc_send(source,
                        g_block_endpoint,
                        BLOCK_IPC_IDENTIFY_RESP,
                        req_id,
                        0,
                        (int32_t)g_unit_sectors[want],
                        (int32_t)want,
                        0);
        return 0;
    }

    /* A transfer does claim one: the first client to ask for a unit keeps it. */
    if (ata_assign_unit_for_source(source, -1, &unit) != 0 || !g_unit_present[unit]) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_UNIT_CLAIMED, 0);
        return 0;
    }

    if (type == BLOCK_IPC_FLUSH_REQ) {
        if (ata_flush_unit(unit) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_WRITE_FAILED, 0);
            return 0;
        }
        ata_send_resp(source, req_id, BLOCK_IPC_FLUSH_RESP, 0, 0);
        return 0;
    }

    if (type == BLOCK_IPC_READ_REQ) {
        int32_t dma_rc = WASMOS_ERR_DMA_DENY;
        int32_t dma_addr = 0;
        uint32_t byte_count = 0;
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_BAD_REQUEST, 0);
            return 0;
        }
        byte_count = (uint32_t)arg2 * ATA_SECTOR_SIZE;
        dma_rc = ata_dma_prepare(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE, &dma_addr);
        if (dma_rc == WASMOS_ERR_NONE) {
            (void)dma_addr;
            ata_log_dma_active(0);
        }
        /* The block buffer is named by physical address, so the controller can
         * write it directly -- no CPU copy on either side. PIO remains the
         * fallback for every case DMA declines. */
        if (ata_read_lba28_dma(unit, (uint32_t)arg1, (uint8_t)arg2, (uint32_t)arg0) == 0) {
            wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_READ_RESP, req_id, 0, arg2, 0, 0);
            return 0;
        }
        ata_sink_t sink = {0u, arg0, 0u};
        if (ata_read_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, &sink) != 0) {
            if (dma_rc == WASMOS_ERR_NONE) {
                (void)ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE);
            }
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_READ_FAILED, 0);
            return 0;
        }
        if (dma_rc == WASMOS_ERR_NONE &&
            ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_READ_FAILED, 0);
            return 0;
        }
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_READ_RESP, req_id, 0, arg2, 0, 0);
        return 0;
    }

    /* Zero-copy read: land whole sectors straight in the client's transfer
     * buffer. The requester reborrowed it to this driver, so the kernel admits
     * both routes to it -- the object write and the borrow mapping -- without
     * the driver ever learning whose buffer it is. arg0 = buffer_id, arg1 = lba,
     * arg2 = (borrow_id << 12) | sector count, arg3 = dst byte offset. */
    if (type == BLOCK_IPC_READ_ZC_REQ) {
        int32_t count = arg2 & (int32_t)WASMOS_BLOCK_ZC_COUNT_MASK;
        int32_t borrow = (int32_t)((uint32_t)arg2 >> WASMOS_BLOCK_ZC_BORROW_SHIFT);
        if (count <= 0 || count > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0 || arg3 < 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_BAD_REQUEST, 0);
            return 0;
        }
        if (ata_read_zc_dma(unit, (uint32_t)arg1, (uint8_t)count, borrow, (uint32_t)arg3) != 0) {
            /* Reached the client's buffer, but by copying into it. Worth saying
             * which of the two it is: both are "zero-copy" as far as the block
             * buffer is concerned, yet only the DMA one is actually copy-free. */
            ata_sink_t sink = {1u, arg0, (uint32_t)arg3};
            if (!g_zc_logged) {
                g_zc_logged = 1;
                (void)printf("[ata] zero-copy reads: staged copy into client buffer\n");
            }
            if (ata_read_lba28(unit, (uint32_t)arg1, (uint8_t)count, &sink) != 0) {
                ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_READ_FAILED, 0);
                return 0;
            }
        }
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_READ_RESP, req_id, 0, count, 0, 0);
        return 0;
    }

    if (type == BLOCK_IPC_WRITE_REQ) {
        int32_t dma_rc = WASMOS_ERR_DMA_DENY;
        int32_t dma_addr = 0;
        uint32_t byte_count = 0;
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_BAD_REQUEST, 0);
            return 0;
        }
        byte_count = (uint32_t)arg2 * ATA_SECTOR_SIZE;
        dma_rc = ata_dma_prepare(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE, &dma_addr);
        if (dma_rc == WASMOS_ERR_NONE) {
            (void)dma_addr;
            ata_log_dma_active(1);
        }
        if (ata_write_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, (uint32_t)arg0) != 0) {
            if (dma_rc == WASMOS_ERR_NONE) {
                (void)ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE);
            }
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_WRITE_FAILED, 0);
            return 0;
        }
        if (dma_rc == WASMOS_ERR_NONE &&
            ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_WRITE_FAILED, 0);
            return 0;
        }
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_WRITE_RESP, req_id, 0, arg2, 0, 0);
        return 0;
    }

    ata_send_resp(source, req_id, BLOCK_IPC_ERROR, WASMOS_ERR_BLOCK_DEV_UNSUPPORTED_REQUEST, 0);
    return 0;
}

/* Driver entry point: create the block endpoint, identify the attached units,
 * register each present one under the "block" service class, route IRQ 14,
 * notify ready, then serve BLOCK_IPC_* requests forever.
 *
 * All four parameters are ignored, including proc_endpoint -- it is overwritten
 * from the spawn-info contract on the first line, because the entry arguments
 * are passed as zero.
 *
 * On success this does not return: the request loop is unbounded. A return is
 * therefore always a bring-up failure.
 * TODO: the failure paths return a bare -1 rather than a packed
 * WASMOS_ERR_DRIVER_* code from abi/errors.yaml, so a caller cannot tell which
 * step failed. The AssemblyScript drivers already return packed codes here. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* proc.endpoint comes from the spawn-info contract, not an entry arg. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();

    g_block_endpoint = wasmos_ipc_create_endpoint();
    if (g_block_endpoint < 0) {
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    g_proc_endpoint_cached = proc_endpoint;
    /* No plain "block" NAME is claimed. A name resolves to ONE provider for the
     * whole system, so holding it made this controller the only disk any client
     * could find -- the class is the discovery path, and this driver joins it
     * once per present drive in ata_register_block_class(). */
    g_devmgr_endpoint = -1;
    for (int32_t attempts = 0; attempts < 256; ++attempts) {
        g_devmgr_endpoint =
            wasmos_svc_lookup(proc_endpoint, g_block_endpoint, "devmgr.inv", 1 + attempts);
        if (g_devmgr_endpoint >= 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    g_present = 0;
    g_sector_count = 0;
    for (uint32_t i = 0; i < ATA_UNIT_COUNT; ++i) {
        g_unit_present[i] = 0;
        g_unit_sectors[i] = 0;
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        g_client_owner[i] = -1;
        g_client_claimed[i] = 0;
        g_client_unit[i] = 0;
    }
    /* Quiet every channel first. The secondary's line is never routed, so a
     * drive there must not assert it; the primary's nIEN is cleared later, only
     * if its route succeeds. */
    for (uint32_t ch = 0; ch < ATA_CHANNEL_COUNT; ++ch) {
        g_channel = ch;
        wasmos_io_region_out8(ata_region(), ATA_REG_CTRL, ATA_CTRL_NIEN);
    }
    for (uint8_t unit = 0; unit < ATA_UNIT_COUNT; ++unit) {
        uint16_t identify_words[256];
        uint8_t unit_present = 0;
        uint32_t unit_sectors = 0;
        if (ata_identify_unit(unit, identify_words) == 0) {
            uint32_t lba28 = ((uint32_t)identify_words[61] << 16) | identify_words[60];
            unit_sectors = lba28;
            unit_present = 1;
            if (unit == 0) {
                g_sector_count = unit_sectors;
                g_present = 1;
            }
        }
        g_unit_present[unit] = unit_present;
        g_unit_sectors[unit] = unit_sectors;
        if (unit_present) {
            g_present = 1;
        }
        ata_publish_block_device(unit, unit_sectors, unit_present);
        ata_register_block_class(unit, unit_present);
    }

    /* The driver addresses its device by region index and never sees a port, so
     * name what each region is for; device-manager logs the windows it granted.
     * The indices are stated rather than read from ata_region(), which holds
     * whichever channel was selected last. */
    (void)printf("[ata] io regions 0/1 = primary/secondary task file (+%02X status, +%03X "
                 "control), irq line %u on the primary\n",
                 (unsigned)ATA_REG_STATUS,
                 (unsigned)ATA_REG_CTRL,
                 (unsigned)ATA_IRQ_LINE);
    ata_dma_setup();
    ata_log_transfer_mode();

    /* Identify ran polled above; from here transfers can be interrupt-driven.
     * Both halves are needed and in this order: route the line first so an
     * assertion has somewhere to go, then clear nIEN to let the drive assert at
     * all. If either fails the driver keeps working exactly as before — polled —
     * so this is an optimisation, never a dependency of the boot path. */
    if (g_present) {
        g_irq_endpoint = wasmos_ipc_create_endpoint();
        g_irq_select = (g_irq_endpoint >= 0) ? wasmos_ipc_select_create() : -1;
        if (g_irq_endpoint >= 0 && g_irq_select >= 0 &&
            wasmos_ipc_select_add(g_irq_select, g_irq_endpoint) == 0 &&
            wasmos_irq_route_ipc((int32_t)ATA_IRQ_LINE, g_irq_endpoint) == 0) {
            g_channel = 0u;
            wasmos_io_region_out8(ata_region(), ATA_REG_CTRL, 0); /* clear nIEN */
            g_irq_active = 1;
            (void)printf("[ata] irq routed line=%u\n", (unsigned)ATA_IRQ_LINE);
        } else {
            (void)printf("[ata] irq route failed line=%u; using polled transfers\n",
                         (unsigned)ATA_IRQ_LINE);
        }
    }

    /* Drivers are long-running processes: initialize once, then block in the
     * IPC loop forever. */
    wasmos_sys_notify_ready(proc_endpoint, g_block_endpoint);
    for (;;) {
        int32_t recv_rc = wasmos_ipc_select_one(g_block_endpoint);
        if (recv_rc < 0) {
            continue;
        }
        /* Capture the request BEFORE touching the interrupt endpoint: draining a
         * message overwrites the shared last-message fields, so servicing the
         * IRQ first would hand ata_handle_ipc the IRQ event's type and source
         * instead of the block request's. */
        int32_t req_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);

        /* Settle anything the previous command left behind. A completion that
         * lands just after a transfer returned would otherwise sit unacked, and
         * the line stays masked until someone acks it. */
        (void)ata_service_irq();

        ata_handle_ipc(req_type, source, req_id, arg0, arg1, arg2, arg3);
    }
    return 0;
}
