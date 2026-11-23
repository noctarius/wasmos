/* acpi_bus.c - WASM service: walks the ACPI tables, finds the namespace devices
 * it recognises (ISA/PNP devices on x86_64, the PL011 UART on aarch64) and
 * publishes each to device-manager as a wasmos_acpi_device_desc_t
 * (DEVMGR_PUBLISH_ACPI_DEVICE_DESC).
 *
 * acpi-bus is the CLIENT of that exchange, so it owns the descriptor buffer and
 * lends it to device-manager (docs/architecture/12-dma-transfers.md). It
 * therefore stays resident after the scan: its exit would destroy the buffer
 * while device-manager may still be reading descriptors from it. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/* ACPI SDT header layout (first 36 bytes of every table). */
typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_hdr_t;

/* ACPI RSDP (v2) layout. */
typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

/* Window in WASM linear memory used for physical page mapping. */
#define PHYS_MAP_WINDOW_PAGES 16
#define PHYS_MAP_WINDOW_SIZE (PHYS_MAP_WINDOW_PAGES * 4096)

/* Destination of every wasmos_phys_map call below. The 4096 alignment is
 * mandatory: phys_map requires a page-aligned wasm_offset (abi/hostcalls.yaml). */
static uint8_t g_map_window[PHYS_MAP_WINDOW_SIZE] __attribute__((aligned(4096)));

/* Map one 4 KB-aligned physical region into g_map_window.
 * phys must be 4096-aligned; size <= PHYS_MAP_WINDOW_SIZE. */
static int map_phys(uint64_t phys, uint32_t size) {
    if ((phys & 0xFFF) != 0 || size == 0 || size > PHYS_MAP_WINDOW_SIZE) {
        return -1;
    }
    uint32_t aligned_size = (size + 0xFFFu) & ~0xFFFu;
    if (aligned_size > PHYS_MAP_WINDOW_SIZE) {
        return -1;
    }
    int32_t lo = (int32_t)(uint32_t)(phys & 0xFFFFFFFFu);
    int32_t hi = (int32_t)(uint32_t)(phys >> 32);
    int32_t off = addr_cast(int32_t, g_map_window);
    return wasmos_phys_map(lo, hi, (int32_t)aligned_size, off);
}

/* ---- Recognised device table -------------------------------------------- */

/* Maps a _HID onto the PCI-like class/subclass/prog_if the device-manager rules
 * match on. An EISAID _HID is matched in its string form ("PNP0501").
 * default_io: I/O window (DEFAULT_IO_PORTS ports) to report when _CRS names
 * none (0 = none); the PS/2 mouse shares the i8042 with the keyboard and lists
 * no ports of its own. */
typedef struct {
    const char* hid;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t default_io;
} acpi_id_entry_t;

static const acpi_id_entry_t acpi_id_table[] = {
    {"PNP0501", 0x07, 0x00, 0x02, 0x0000},  /* 16550A serial                 */
    {"ARMH0011", 0x07, 0x00, 0x80, 0x0000}, /* Arm PL011 UART                */
    {"PNP0303", 0x09, 0x00, 0x00, 0x0000},  /* AT keyboard (i8042)           */
    {"PNP0F03", 0x09, 0x02, 0x00, 0x0060},  /* MS serial mouse               */
    {"PNP0F13", 0x09, 0x02, 0x00, 0x0060},  /* PS/2 mouse (i8042)            */
    {"PNP0700", 0x01, 0x02, 0x00, 0x0000},  /* floppy controller             */
    {"PNP0400", 0x07, 0x01, 0x00, 0x0000},  /* parallel port ECP             */
    {"PNP0401", 0x07, 0x01, 0x01, 0x0000},  /* parallel port EPP             */
    {"PNP0B00", 0x08, 0x03, 0x00, 0x0000},  /* CMOS RTC                      */
    {"PNP0C04", 0x0B, 0x80, 0x00, 0x0000},  /* FPU/coprocessor               */
    {"PNP0C02", 0x08, 0x80, 0x00, 0x0000},  /* motherboard resources         */
};
#define ACPI_ID_TABLE_LEN (sizeof(acpi_id_table) / sizeof(acpi_id_table[0]))

/* Ports in a default_io window: the i8042's data (0x60) and command/status
 * (0x64) registers both fall inside it. */
#define DEFAULT_IO_PORTS 8u

/* Descriptors published in one scan; a device past the last slot is not
 * published. */
#define ACPI_MAX_PUBLISHED_DEVICES 32u

/* RSDT/XSDT entries walked; firmware tables list a few dozen at most. */
#define ACPI_MAX_SDT_ENTRIES 64u

static const acpi_id_entry_t* acpi_lookup(const char* hid) {
    for (uint32_t k = 0; k < ACPI_ID_TABLE_LEN; k++) {
        if (strcmp(acpi_id_table[k].hid, hid) == 0) {
            return &acpi_id_table[k];
        }
    }
    return NULL;
}

/* ---- AML helper functions ------------------------------------------------ */

/* Decode an AML package-length field; returns bytes consumed in *consumed. */
static int aml_pkglen(const uint8_t* buf, uint32_t avail, uint32_t* out_len, uint32_t* consumed) {
    if (avail < 1) {
        return -1;
    }
    uint8_t b0 = buf[0];
    uint32_t follow = (uint32_t)(b0 >> 6) & 0x3u;
    if (avail < follow + 1u) {
        return -1;
    }
    uint32_t len = (uint32_t)(b0 & (follow ? 0x0Fu : 0x3Fu));
    for (uint32_t i = 0; i < follow; i++) {
        len |= (uint32_t)buf[1u + i] << (4u + i * 8u);
    }
    *out_len = len;
    *consumed = follow + 1u;
    return 0;
}

/* Read one AML integer operand (ZeroOp, OneOp, Byte/Word/DWordConst) into
 * *value; returns bytes consumed or -1. */
static int aml_read_int(const uint8_t* buf, uint32_t avail, uint32_t* value) {
    if (avail < 1) {
        return -1;
    }
    switch (buf[0]) {
    case 0x00: /* ZeroOp */
        *value = 0;
        return 1;
    case 0x01: /* OneOp */
        *value = 1;
        return 1;
    case 0x0A: /* ByteConst */
        if (avail < 2) {
            return -1;
        }
        *value = buf[1];
        return 2;
    case 0x0B: /* WordConst */
        if (avail < 3) {
            return -1;
        }
        *value = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8);
        return 3;
    case 0x0C: /* DWordConst */
        if (avail < 5) {
            return -1;
        }
        *value = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16) |
                 ((uint32_t)buf[4] << 24);
        return 5;
    default:
        return -1;
    }
}

static uint32_t read_le16(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t* p) {
    return read_le16(p) | (read_le16(p + 2) << 16);
}

/* Write the string form of an EISAID (ACPI 6.5 §19.6.35): three compressed
 * vendor letters in the first two bytes, four hex digits in the last two. */
static void eisaid_to_hid(const uint8_t* id, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t vendor = ((uint32_t)id[0] << 8) | id[1];
    out[0] = (char)('@' + ((vendor >> 10) & 0x1Fu));
    out[1] = (char)('@' + ((vendor >> 5) & 0x1Fu));
    out[2] = (char)('@' + (vendor & 0x1Fu));
    out[3] = hex[id[2] >> 4];
    out[4] = hex[id[2] & 0xFu];
    out[5] = hex[id[3] >> 4];
    out[6] = hex[id[3] & 0xFu];
    out[7] = '\0';
}

/* The value of a `Name (_HID, ...)` whose NameOp starts at aml[i], as a string:
 * an EISAID (DWordConst) is converted, a string is copied. Returns the bytes
 * the object occupies, or 0 when aml[i] does not start a usable _HID. */
static uint32_t read_hid(const uint8_t* aml, uint32_t aml_len, uint32_t i, char* hid) {
    if (i + 6u > aml_len || aml[i] != 0x08 || aml[i + 1] != '_' || aml[i + 2] != 'H' ||
        aml[i + 3] != 'I' || aml[i + 4] != 'D') {
        return 0;
    }
    uint32_t pos = i + 5u;
    if (aml[pos] == 0x0C) {
        if (pos + 5u > aml_len) {
            return 0;
        }
        eisaid_to_hid(aml + pos + 1u, hid);
        return 10u;
    }
    if (aml[pos] == 0x0D) {
        uint32_t n = 0;
        pos++;
        while (pos + n < aml_len && aml[pos + n] != 0 && n + 1u < WASMOS_ACPI_HID_MAX) {
            hid[n] = (char)aml[pos + n];
            n++;
        }
        if (pos + n >= aml_len || aml[pos + n] != 0) {
            return 0;
        }
        hid[n] = '\0';
        return 7u + n;
    }
    return 0;
}

/* Append one register window to the descriptor; windows past
 * WASMOS_REGION_LIMIT are dropped. */
static void add_window(wasmos_acpi_device_desc_t* desc, uint32_t kind, uint64_t base,
                       uint32_t length) {
    if (length == 0 || desc->window_count >= WASMOS_REGION_LIMIT) {
        return;
    }
    wasmos_region_grant_t* w = &desc->windows[desc->window_count++];
    w->kind = kind;
    w->length = length;
    w->base = base;
}

/* Fill the descriptor's windows and interrupt from the resource template of a
 * `Name (_CRS, Buffer (...) {...})` found within `window` bytes after aml[from].
 * Every I/O and fixed memory window is kept, in template order, and the first
 * interrupt. Descriptors are walked by their encoded lengths (ACPI 6.5 §6.4),
 * small and large alike. Returns 0, or -1 when no such _CRS is found. */
static int read_crs(const uint8_t* aml, uint32_t aml_len, uint32_t from, uint32_t window,
                    wasmos_acpi_device_desc_t* desc) {
    uint32_t limit = aml_len - from < window ? aml_len - from : window;
    uint32_t i = from;
    for (; i + 6u <= from + limit; i++) {
        if (aml[i] == 0x08 && aml[i + 1] == '_' && aml[i + 2] == 'C' && aml[i + 3] == 'R' &&
            aml[i + 4] == 'S' && aml[i + 5] == 0x11) {
            break;
        }
    }
    if (i + 6u > from + limit) {
        return -1;
    }
    uint32_t pos = i + 6u;
    uint32_t pkg_len = 0;
    uint32_t consumed = 0;
    if (aml_pkglen(aml + pos, aml_len - pos, &pkg_len, &consumed) != 0) {
        return -1;
    }
    pos += consumed;
    uint32_t buf_len = 0;
    int n = aml_read_int(aml + pos, aml_len - pos, &buf_len);
    if (n < 0) {
        return -1;
    }
    pos += (uint32_t)n;
    uint32_t end = pos + buf_len;
    if (end > aml_len) {
        return -1;
    }
    while (pos < end) {
        uint8_t tag = aml[pos];
        uint32_t len;
        uint32_t body;
        if (tag & 0x80u) {
            if (pos + 3u > end) {
                return 0;
            }
            len = read_le16(aml + pos + 1u);
            body = pos + 3u;
        } else {
            len = tag & 0x07u;
            body = pos + 1u;
        }
        if (body + len > end) {
            return 0;
        }
        if (!(tag & 0x80u)) {
            uint32_t type = (uint32_t)(tag >> 3) & 0x0Fu;
            if (type == 0x0Fu) {
                return 0; /* End Tag */
            }
            if (type == 0x04u && len >= 2u && desc->irq == WASMOS_ACPI_NO_IRQ) {
                /* IRQ: a 16-bit mask of ISA lines; the lowest set one. */
                uint32_t mask = read_le16(aml + body);
                for (uint32_t line = 0; line < 16u; line++) {
                    if (mask & (1u << line)) {
                        desc->irq = line;
                        break;
                    }
                }
            } else if (type == 0x08u && len >= 7u) {
                /* I/O Port: minimum base at +1, length at +6. */
                add_window(
                    desc, WASMOS_REGION_GRANT_IO, read_le16(aml + body + 1u), aml[body + 6u]);
            } else if (type == 0x09u && len >= 3u) {
                /* Fixed I/O Port: 10-bit base at +0, length at +2. */
                add_window(
                    desc, WASMOS_REGION_GRANT_IO, read_le16(aml + body) & 0x3FFu, aml[body + 2u]);
            }
        } else if (tag == 0x86u && len >= 9u) {
            /* 32-bit Fixed Memory Range: base at +1, length at +5. */
            add_window(desc,
                       WASMOS_REGION_GRANT_MMIO,
                       read_le32(aml + body + 1u),
                       read_le32(aml + body + 5u));
        } else if (tag == 0x89u && len >= 6u && desc->irq == WASMOS_ACPI_NO_IRQ) {
            /* Extended Interrupt: count at +1, the interrupts from +2. */
            if (aml[body + 1u] > 0) {
                desc->irq = read_le32(aml + body + 2u);
            }
        }
        pos = body + len;
    }
    return 0;
}

/* ---- Device scan ---------------------------------------------------------- */

/* The console UART the firmware names in its SPCR table: the address space
 * (0 = memory, 1 = I/O) and base address of its register window. */
typedef struct {
    int present;
    uint8_t space;
    uint64_t address;
} acpi_console_t;

/* Scan the AML for `Name (_HID, ...)` objects with a resource template and
 * publish each recognised device as a descriptor in slot order of `buffer_id`,
 * which acpi-bus owns and has lent to device-manager. A device whose register
 * window is the SPCR console's is flagged WASMOS_ACPI_DEVICE_CONSOLE. Returns
 * the number of devices published. */
static uint32_t scan_devices(const uint8_t* aml, uint32_t aml_len, const acpi_console_t* console,
                             int32_t devmgr_ep, int32_t src_ep, int32_t buffer_id,
                             int32_t* req_id) {
    uint32_t published = 0;
    /* QEMU's virt lists 32 virtio-mmio slots under one _HID; an unknown _HID is
     * reported once, not once per device. */
    char last_unknown[WASMOS_ACPI_HID_MAX] = {0};
    for (uint32_t i = 0; i + 6u <= aml_len; i++) {
        char hid[WASMOS_ACPI_HID_MAX];
        uint32_t hid_len = read_hid(aml, aml_len, i, hid);
        if (hid_len == 0) {
            continue;
        }
        const acpi_id_entry_t* entry = acpi_lookup(hid);
        if (!entry) {
            if (strcmp(hid, last_unknown) != 0) {
                (void)printf("[acpi-bus] unknown %s, skipping\n", hid);
                str_copy(last_unknown, sizeof(last_unknown), hid);
            }
            continue;
        }
        wasmos_acpi_device_desc_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.version = WASMOS_ACPI_DEVICE_DESC_VERSION;
        desc.class_code = entry->class_code;
        desc.subclass = entry->subclass;
        desc.prog_if = entry->prog_if;
        desc.irq = WASMOS_ACPI_NO_IRQ;
        str_copy(desc.hid, sizeof(desc.hid), hid);
        if (read_crs(aml, aml_len, i + hid_len, 512u, &desc) != 0) {
            continue;
        }
        if (desc.window_count == 0 && entry->default_io != 0) {
            add_window(&desc, WASMOS_REGION_GRANT_IO, entry->default_io, DEFAULT_IO_PORTS);
        }
        if (desc.window_count == 0) {
            continue;
        }
        for (uint32_t w = 0; console->present && w < desc.window_count; w++) {
            uint32_t want = console->space == 0 ? WASMOS_REGION_GRANT_MMIO : WASMOS_REGION_GRANT_IO;
            if (desc.windows[w].kind == want && desc.windows[w].base == console->address) {
                desc.flags |= WASMOS_ACPI_DEVICE_CONSOLE;
            }
        }
        if (published >= ACPI_MAX_PUBLISHED_DEVICES) {
            (void)printf("[acpi-bus] %s not published: descriptor slots full\n", hid);
            continue;
        }
        (void)printf("[acpi-bus] %s %s=0x%X irq=%d windows=%u class=0x%02X%s\n",
                     hid,
                     desc.windows[0].kind == WASMOS_REGION_GRANT_IO ? "io" : "mmio",
                     (unsigned)desc.windows[0].base,
                     desc.irq == WASMOS_ACPI_NO_IRQ ? -1 : (int)desc.irq,
                     (unsigned)desc.window_count,
                     (unsigned)desc.class_code,
                     (desc.flags & WASMOS_ACPI_DEVICE_CONSOLE) ? " console" : "");
        uint32_t offset = published * (uint32_t)sizeof(desc);
        if (wasmos_xfer_buffer_write(buffer_id, &desc, (int32_t)sizeof(desc), (int32_t)offset) !=
            0) {
            (void)printf("[acpi-bus] descriptor write failed\n");
            continue;
        }
        (void)wasmos_ipc_send(devmgr_ep,
                              src_ep,
                              DEVMGR_PUBLISH_ACPI_DEVICE_DESC,
                              (*req_id)++,
                              buffer_id,
                              (int32_t)offset,
                              (int32_t)sizeof(desc),
                              0);
        published++;
    }
    return published;
}

/* ---- Main entry point ---------------------------------------------------- */

/* Service entry point called by PM.
 * Walks RSDP -> RSDT/XSDT -> FACP -> DSDT, maps each physical table into
 * g_map_window via wasmos_phys_map, notes the SPCR console on the way, then
 * publishes the devices scan_devices finds in the AML. Sends
 * DEVMGR_ACPI_SCAN_DONE to device-manager when finished (success or failure) so
 * it never stalls waiting for the scan, signals readiness to PM, and then stays
 * parked on its endpoint (see the file header). */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* The proc endpoint comes from the spawn-info contract; the entry args carry
     * nothing and the parameter is overwritten. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    if (proc_endpoint < 0) {
        return -1;
    }

    int32_t src_ep = wasmos_ipc_create_endpoint();
    if (src_ep < 0) {
        return -1;
    }
    int32_t devmgr_ep = wasmos_sys_svc_lookup_retry(proc_endpoint, src_ep, "devmgr.inv", 1, 1024);
    if (devmgr_ep < 0) {
        return -1;
    }
    int32_t req_id = 1;
    acpi_console_t console = {0};

    /* One buffer, one slot per device, lent to device-manager for reading. */
    int32_t desc_bid = wasmos_xfer_buffer_acquire(
        (int32_t)(ACPI_MAX_PUBLISHED_DEVICES * sizeof(wasmos_acpi_device_desc_t)));
    if (desc_bid < 0 ||
        wasmos_xfer_buffer_borrow(devmgr_ep, desc_bid, WASMOS_BUFFER_GRANT_READ) < 0) {
        (void)printf("[acpi-bus] descriptor buffer unavailable\n");
        goto done;
    }

    /* --- Step 1: get RSDP ------------------------------------------------- */
    acpi_rsdp_t rsdp;
    uint32_t rsdp_len = 0;
    if (wasmos_acpi_rsdp_info(
            addr_cast(int32_t, &rsdp), addr_cast(int32_t, &rsdp_len), (int32_t)sizeof(rsdp)) != 0) {
        (void)printf("[acpi-bus] RSDP not found\n");
        goto done;
    }
    if (wasmos_sync_user_read(addr_cast(int32_t, &rsdp_len), (int32_t)sizeof(rsdp_len)) != 0 ||
        wasmos_sync_user_read(addr_cast(int32_t, &rsdp), (int32_t)sizeof(acpi_rsdp_t)) != 0) {
        (void)printf("[acpi-bus] RSDP sync failed\n");
        goto done;
    }

    /* --- Step 2: locate DSDT via RSDT/XSDT -> FACP ------------------------ */
    int use_xsdt = (rsdp.revision >= 2 && rsdp.xsdt_address != 0);
    uint64_t sdt_phys = use_xsdt ? rsdp.xsdt_address : (uint64_t)rsdp.rsdt_address;

    /* Map first page of RSDT/XSDT to read its header. */
    if (map_phys(sdt_phys & ~(uint64_t)0xFFF, 4096) != 0) {
        (void)printf("[acpi-bus] RSDT/XSDT map failed\n");
        goto done;
    }
    uint32_t page_off = (uint32_t)(sdt_phys & 0xFFF);
    const acpi_sdt_hdr_t* sdt_hdr = (const acpi_sdt_hdr_t*)(g_map_window + page_off);
    uint32_t sdt_len = sdt_hdr->length;
    if (sdt_len < 36u || sdt_len > PHYS_MAP_WINDOW_SIZE - page_off) {
        (void)printf("[acpi-bus] RSDT/XSDT length invalid\n");
        goto done;
    }

    /* Map full RSDT/XSDT if it spans beyond the first page. */
    if (sdt_len + page_off > 4096u) {
        uint32_t full_size = (sdt_len + page_off + 0xFFFu) & ~0xFFFu;
        if (map_phys(sdt_phys & ~(uint64_t)0xFFF, full_size) != 0) {
            (void)printf("[acpi-bus] RSDT/XSDT full map failed\n");
            goto done;
        }
        sdt_hdr = (const acpi_sdt_hdr_t*)(g_map_window + page_off);
    }

    /* Walk RSDT/XSDT entries for FACP and SPCR. The entry list is copied out
     * first: peeking at each table remaps g_map_window, which holds the list. */
    uint32_t entry_size = use_xsdt ? 8u : 4u;
    uint32_t n_entries = (sdt_len - 36u) / entry_size;
    const uint8_t* entries = (const uint8_t*)sdt_hdr + 36u;
    uint64_t table_phys[ACPI_MAX_SDT_ENTRIES];
    if (n_entries > ACPI_MAX_SDT_ENTRIES) {
        n_entries = ACPI_MAX_SDT_ENTRIES;
    }
    for (uint32_t i = 0; i < n_entries; i++) {
        table_phys[i] = 0;
        if (use_xsdt) {
            __builtin_memcpy(&table_phys[i], entries + i * 8u, 8u);
        } else {
            uint32_t ep32 = 0;
            __builtin_memcpy(&ep32, entries + i * 4u, 4u);
            table_phys[i] = (uint64_t)ep32;
        }
    }
    uint64_t facp_phys = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        uint64_t ep = table_phys[i];
        /* Peek at the table signature: map one page to check. */
        uint64_t ep_page = ep & ~(uint64_t)0xFFF;
        uint32_t ep_off = (uint32_t)(ep & 0xFFF);
        if (map_phys(ep_page, 4096) != 0) {
            continue;
        }
        const acpi_sdt_hdr_t* hdr = (const acpi_sdt_hdr_t*)(g_map_window + ep_off);
        if (hdr->signature[0] == 'F' && hdr->signature[1] == 'A' && hdr->signature[2] == 'C' &&
            hdr->signature[3] == 'P') {
            facp_phys = ep;
        } else if (hdr->signature[0] == 'S' && hdr->signature[1] == 'P' &&
                   hdr->signature[2] == 'C' && hdr->signature[3] == 'R' && hdr->length >= 52u &&
                   ep_off + 52u <= 4096u) {
            /* SPCR: the console's Generic Address Structure at offset 40 --
             * address space at +0, 64-bit address at +4. */
            const uint8_t* gas = (const uint8_t*)hdr + 40u;
            console.present = 1;
            console.space = gas[0];
            __builtin_memcpy(&console.address, gas + 4, 8u);
        }
    }
    if (facp_phys == 0) {
        (void)printf("[acpi-bus] FACP not found\n");
        goto done;
    }

    /* Map FACP to extract DSDT pointer. */
    uint64_t facp_page = facp_phys & ~(uint64_t)0xFFF;
    uint32_t facp_off = (uint32_t)(facp_phys & 0xFFF);
    if (map_phys(facp_page, 4096) != 0) {
        (void)printf("[acpi-bus] FACP map failed\n");
        goto done;
    }
    const uint8_t* facp = g_map_window + facp_off;
    const acpi_sdt_hdr_t* facp_hdr = (const acpi_sdt_hdr_t*)facp;
    uint64_t dsdt_phys = 0;
    if (facp_hdr->length >= 148u) {
        __builtin_memcpy(&dsdt_phys, facp + 140u, 8u);
    }
    if (dsdt_phys == 0 && facp_hdr->length >= 44u) {
        uint32_t dp32 = 0;
        __builtin_memcpy(&dp32, facp + 40u, 4u);
        dsdt_phys = (uint64_t)dp32;
    }
    if (dsdt_phys == 0) {
        (void)printf("[acpi-bus] DSDT pointer missing in FACP\n");
        goto done;
    }

    /* Map DSDT header (first page) to get length. */
    uint64_t dsdt_page = dsdt_phys & ~(uint64_t)0xFFF;
    uint32_t dsdt_off = (uint32_t)(dsdt_phys & 0xFFF);
    if (map_phys(dsdt_page, 4096) != 0) {
        (void)printf("[acpi-bus] DSDT map failed\n");
        goto done;
    }
    const acpi_sdt_hdr_t* dsdt_hdr = (const acpi_sdt_hdr_t*)(g_map_window + dsdt_off);
    if (dsdt_hdr->signature[0] != 'D' || dsdt_hdr->signature[1] != 'S' ||
        dsdt_hdr->signature[2] != 'D' || dsdt_hdr->signature[3] != 'T') {
        (void)printf("[acpi-bus] DSDT signature mismatch\n");
        goto done;
    }
    uint32_t dsdt_len = dsdt_hdr->length;
    if (dsdt_len < 36u || dsdt_len > PHYS_MAP_WINDOW_SIZE) {
        (void)printf("[acpi-bus] DSDT length out of range\n");
        goto done;
    }
    (void)printf("[acpi-bus] DSDT %u bytes\n", (unsigned)dsdt_len);

    /* Map the full DSDT. */
    uint32_t dsdt_map_size = (dsdt_len + dsdt_off + 0xFFFu) & ~0xFFFu;
    if (dsdt_map_size > PHYS_MAP_WINDOW_SIZE) {
        (void)printf("[acpi-bus] DSDT too large for window\n");
        goto done;
    }
    if (map_phys(dsdt_page, dsdt_map_size) != 0) {
        (void)printf("[acpi-bus] DSDT full map failed\n");
        goto done;
    }

    /* AML starts after the 36-byte ACPI header. */
    const uint8_t* aml = g_map_window + dsdt_off + 36u;
    uint32_t aml_len = dsdt_len - 36u;
    (void)scan_devices(aml, aml_len, &console, devmgr_ep, src_ep, desc_bid, &req_id);

done:
    (void)wasmos_ipc_send(devmgr_ep, src_ep, DEVMGR_ACPI_SCAN_DONE, req_id, 0, 0, 0, 0);
    wasmos_sys_notify_ready(proc_endpoint, src_ep);
    /* Resident from here on: nothing is served, and the process stays only so
     * the descriptor buffer outlives device-manager's reads of it. */
    for (;;) {
        (void)wasmos_ipc_select_one(src_ep);
    }
}
