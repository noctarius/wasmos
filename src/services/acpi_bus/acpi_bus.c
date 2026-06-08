/* acpi_bus.c - WASM service: walks ACPI tables, extracts ISA/PNP devices,
 * publishes them to device-manager via DEVMGR_PUBLISH_DEVICE IPC messages. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"

/* ACPI SDT header layout (first 36 bytes of every table). */
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_hdr_t;

/* ACPI RSDP (v2) layout. */
typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

/* Window in WASM linear memory used for physical page mapping. */
#define PHYS_MAP_WINDOW_PAGES 16
#define PHYS_MAP_WINDOW_SIZE  (PHYS_MAP_WINDOW_PAGES * 4096)

/* Page-aligned mapping window — must sit on a page boundary in WASM memory.
 * Declared with aligned attribute; the linker will place it on a page boundary. */
static uint8_t g_map_window[PHYS_MAP_WINDOW_SIZE] __attribute__((aligned(4096)));

/* Map one 4 KB-aligned physical region into g_map_window.
 * phys must be 4096-aligned; size <= PHYS_MAP_WINDOW_SIZE. */
static int
map_phys(uint64_t phys, uint32_t size)
{
    if ((phys & 0xFFF) != 0 || size == 0 || size > PHYS_MAP_WINDOW_SIZE) {
        return -1;
    }
    uint32_t aligned_size = (size + 0xFFFu) & ~0xFFFu;
    if (aligned_size > PHYS_MAP_WINDOW_SIZE) {
        return -1;
    }
    int32_t lo = (int32_t)(uint32_t)(phys & 0xFFFFFFFFu);
    int32_t hi = (int32_t)(uint32_t)(phys >> 32);
    int32_t off = (int32_t)(uintptr_t)g_map_window;
    return wasmos_phys_map(lo, hi, (int32_t)aligned_size, off);
}

/* ---- ISA/PNP device ID table -------------------------------------------- */

/* Maps EISAID device bytes (vendor is always 0x41 0xD0 for PNP) to PCI-like
 * class/subclass/prog_if so the device-manager rules can match them.
 * default_io: fallback I/O base if _CRS has no I/O descriptor (0 = skip). */
typedef struct {
    uint8_t  b2, b3;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint16_t default_io;
} isa_id_entry_t;

static const isa_id_entry_t isa_id_table[] = {
    { 0x05, 0x01, 0x07, 0x00, 0x02, 0x0000 },  /* PNP0501 - 16550A serial       */
    { 0x03, 0x03, 0x09, 0x00, 0x00, 0x0000 },  /* PNP0303 - AT keyboard (i8042) */
    { 0x0F, 0x03, 0x09, 0x02, 0x00, 0x0060 },  /* PNP0F03 - MS serial mouse     */
    { 0x0F, 0x13, 0x09, 0x02, 0x00, 0x0060 },  /* PNP0F13 - PS/2 mouse (i8042)  */
    { 0x07, 0x00, 0x01, 0x02, 0x00, 0x0000 },  /* PNP0700 - floppy controller   */
    { 0x04, 0x00, 0x07, 0x01, 0x00, 0x0000 },  /* PNP0400 - parallel port ECP   */
    { 0x04, 0x01, 0x07, 0x01, 0x01, 0x0000 },  /* PNP0401 - parallel port EPP   */
    { 0x0B, 0x00, 0x08, 0x03, 0x00, 0x0000 },  /* PNP0B00 - CMOS RTC            */
    { 0x0C, 0x04, 0x0B, 0x80, 0x00, 0x0000 },  /* PNP0C04 - FPU/coprocessor     */
    { 0x0C, 0x02, 0x08, 0x80, 0x00, 0x0000 },  /* PNP0C02 - motherboard rsrcs   */
};
#define ISA_ID_TABLE_LEN (sizeof(isa_id_table) / sizeof(isa_id_table[0]))

/* Look up (b2, b3) EISAID bytes in the static ISA device table;
 * returns NULL if not recognised. */
static const isa_id_entry_t *
isa_lookup(uint8_t b2, uint8_t b3)
{
    for (uint32_t k = 0; k < ISA_ID_TABLE_LEN; k++) {
        if (isa_id_table[k].b2 == b2 && isa_id_table[k].b3 == b3) {
            return &isa_id_table[k];
        }
    }
    return NULL;
}

/* ---- AML helper functions ------------------------------------------------ */

/* Boyer-Moore-style byte-pattern search; returns first match offset or -1. */
static int32_t
find_bytes(const uint8_t *buf, uint32_t len,
           const uint8_t *pat, uint32_t pat_len)
{
    if (pat_len == 0 || len < pat_len) {
        return -1;
    }
    for (uint32_t i = 0; i <= len - pat_len; i++) {
        uint32_t j = 0;
        while (j < pat_len && buf[i + j] == pat[j]) {
            j++;
        }
        if (j == pat_len) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Decode an AML package-length field; returns bytes consumed in *consumed. */
static int
aml_pkglen(const uint8_t *buf, uint32_t avail,
           uint32_t *out_len, uint32_t *consumed)
{
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

/* Skip one AML integer operand; returns bytes consumed or -1. */
static int
aml_skip_int(const uint8_t *buf, uint32_t avail)
{
    if (avail < 1) {
        return -1;
    }
    uint8_t op = buf[0];
    /* Multi-byte opcodes must be checked before the generic <= 0x7F catch. */
    if (op == 0x0A) { return avail >= 2 ? 2 : -1; }  /* ByteConst */
    if (op == 0x0B) { return avail >= 3 ? 3 : -1; }  /* WordConst */
    if (op == 0x0C) { return avail >= 5 ? 5 : -1; }  /* DWordConst */
    if (op == 0x0E) { return avail >= 9 ? 9 : -1; }  /* QWordConst */
    if (op == 0x00 || op == 0x01 || op == 0xFF || op <= 0x7Fu) {
        return 1;
    }
    return -1;
}

/* Return the lowest set bit index of a 16-bit IRQ bitmask, 0xFF if none. */
static uint8_t
irq_from_mask(uint16_t mask)
{
    for (uint8_t i = 0; i < 16u; i++) {
        if (mask & (uint16_t)(1u << i)) {
            return i;
        }
    }
    return 0xFFu;
}

/* ---- Generic AML scanner for all ISA/PNP devices ------------------------- */

/* Scan raw AML bytecode for _HID EISAID / _CRS resource pairs and publish
 * each recognised ISA device to device-manager.
 * IPC encoding for DEVMGR_PUBLISH_DEVICE (bus=0xFF marks ISA, not PCI):
 *   arg0 = (0xFF<<24) | class_code
 *   arg1 = (subclass<<24) | (prog_if<<16)
 *   arg2 = io_base (first I/O port from _CRS or default_io)
 *   arg3 = irq << 8  (0 when IRQ unavailable) */
static void
scan_isa_devices(const uint8_t *aml, uint32_t aml_len,
                 int32_t devmgr_ep, int32_t src_ep, int32_t *req_id)
{
    /* Dedup: skip re-publishing the same physical device (same PNP ID + I/O base).
     * Keyed on (io_base<<16)|(b2<<8)|b3 so different devices sharing I/O ports
     * (e.g. keyboard PNP0303 and mouse PNP0F13 both on 0x60) are kept distinct. */
    uint32_t seen[16];
    uint32_t n_seen = 0;

    for (uint32_t i = 0; i + 5u <= aml_len; i++) {
        /* DWordPrefix (0x0C) + PNP vendor EISAID bytes (0x41 0xD0). */
        if (aml[i] != 0x0Cu || aml[i+1] != 0x41u || aml[i+2] != 0xD0u) {
            continue;
        }

        uint8_t b2 = aml[i+3];
        uint8_t b3 = aml[i+4];

        const isa_id_entry_t *entry = isa_lookup(b2, b3);
        if (!entry) {
            (void)printf("[acpi-bus] unknown PNP%02X%02X, skipping\n",
                         (unsigned)b2, (unsigned)b3);
            continue;
        }

        /* Require _HID name in the 32 bytes before the EISAID. */
        static const uint8_t HID[4] = { '_', 'H', 'I', 'D' };
        uint32_t look_back = (i >= 32u) ? 32u : i;
        if (find_bytes(aml + i - look_back, look_back, HID, 4) < 0) {
            continue;
        }

        /* Find _CRS within 512 bytes after the EISAID. */
        static const uint8_t CRS[4] = { '_', 'C', 'R', 'S' };
        uint32_t fwd = aml_len - i;
        if (fwd > 512u) { fwd = 512u; }
        int32_t crs_off = find_bytes(aml + i, fwd, CRS, 4);
        if (crs_off < 0) {
            continue;
        }

        /* After _CRS expect a BufferOp (0x11). */
        uint32_t pos = (uint32_t)i + (uint32_t)crs_off + 4u;
        if (pos >= aml_len || aml[pos] != 0x11u) {
            continue;
        }
        pos++;

        /* Skip package length then buffer-size integer operand. */
        uint32_t pkg_len = 0, consumed = 0;
        if (pos >= aml_len ||
            aml_pkglen(aml + pos, aml_len - pos, &pkg_len, &consumed) != 0) {
            continue;
        }
        pos += consumed;
        if (pos >= aml_len) {
            continue;
        }
        int skip = aml_skip_int(aml + pos, aml_len - pos);
        if (skip < 0) {
            continue;
        }
        pos += (uint32_t)skip;

        /* Scan resource descriptors for I/O port (0x47) and IRQ (0x22/0x23). */
        uint16_t io_base = 0;
        uint8_t  irq     = 0xFFu;
        uint32_t res_end = aml_len - pos;
        if (res_end > 64u) { res_end = 64u; }

        for (uint32_t r = 0; r + 1u < res_end; r++) {
            uint8_t tag = aml[pos + r];
            if (tag == 0x79u) { break; }  /* EndTag */
            if (tag == 0x47u && r + 8u <= res_end) {
                /* IO Port descriptor: 1 tag + 7 data bytes.
                 * Take the first (lowest) I/O base found. */
                if (io_base == 0) {
                    io_base = (uint16_t)aml[pos+r+2] |
                              ((uint16_t)aml[pos+r+3] << 8);
                }
                r += 7u;
                continue;
            }
            if ((tag == 0x22u || tag == 0x23u) && r + 2u < res_end) {
                /* Small IRQ descriptor: 0x22 = 3 bytes, 0x23 = 4 bytes. */
                uint16_t mask = (uint16_t)aml[pos+r+1] |
                                ((uint16_t)aml[pos+r+2] << 8);
                irq = irq_from_mask(mask);
                r += (tag == 0x23u) ? 3u : 2u;
                continue;
            }
        }

        /* Use the lookup table's default I/O base for devices that share ports
         * with another controller (e.g. PS/2 mouse sharing i8042 at 0x60). */
        if (io_base == 0) {
            if (entry->default_io != 0) {
                io_base = entry->default_io;
            } else {
                continue;
            }
        }

        /* Skip re-publishing the same physical device (_HID + _CID duplicate). */
        uint32_t dedup_key = ((uint32_t)io_base << 16) | ((uint32_t)b2 << 8) | b3;
        uint32_t dup = 0;
        for (uint32_t s = 0; s < n_seen; s++) {
            if (seen[s] == dedup_key) { dup = 1; break; }
        }
        if (dup) {
            continue;
        }
        if (n_seen < 16u) {
            seen[n_seen++] = dedup_key;
        }

        (void)printf("[acpi-bus] PNP%02X%02X io=0x%04X irq=%u class=0x%02X\n",
                     (unsigned)b2, (unsigned)b3,
                     (unsigned)io_base, (unsigned)irq,
                     (unsigned)entry->class_code);

        /* Publish as non-PCI (bus=0xFF) ISA device.
         * arg0: (bus<<24) | class_code
         * arg1: (subclass<<24) | (prog_if<<16) | vendor_id
         * arg2: device_id = io_base (ISA I/O base address)
         * arg3: irq_hint in byte 1 */
        uint32_t a0 = (0xFFu << 24) | (uint32_t)entry->class_code;
        uint32_t a1 = ((uint32_t)entry->subclass << 24) |
                      ((uint32_t)entry->prog_if  << 16);
        uint32_t a2 = (uint32_t)io_base;
        uint32_t a3 = (irq < 16u) ? ((uint32_t)irq << 8) : 0u;
        (void)wasmos_ipc_send(devmgr_ep, src_ep,
                              DEVMGR_PUBLISH_DEVICE, (*req_id)++,
                              (int32_t)a0, (int32_t)a1,
                              (int32_t)a2, (int32_t)a3);
    }
}

/* ---- Main entry point ---------------------------------------------------- */

/* Service entry point called by PM.
 * Walks RSDP -> RSDT/XSDT -> FACP -> DSDT, maps each physical table into
 * g_map_window via wasmos_phys_map, then calls scan_isa_devices on the AML.
 * Sends DEVMGR_ACPI_SCAN_DONE to device-manager when finished (success or
 * failure) so it never stalls waiting for the scan, then calls
 * wasmos_sys_notify_ready to signal full service readiness to PM. */
WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1; (void)ignored_arg2; (void)ignored_arg3;
    if (proc_endpoint < 0) {
        return -1;
    }

    int32_t src_ep = wasmos_ipc_create_endpoint();
    if (src_ep < 0) {
        return -1;
    }
    int32_t devmgr_ep = wasmos_sys_svc_lookup_retry(proc_endpoint, src_ep,
                                                     "devmgr.inv", 1, 1024);
    if (devmgr_ep < 0) {
        return -1;
    }
    int32_t req_id = 1;

    /* --- Step 1: get RSDP ------------------------------------------------- */
    acpi_rsdp_t rsdp;
    uint32_t rsdp_len = 0;
    if (wasmos_acpi_rsdp_info((int32_t)(uintptr_t)&rsdp,
                               (int32_t)(uintptr_t)&rsdp_len,
                               (int32_t)sizeof(rsdp)) != 0) {
        (void)printf("[acpi-bus] RSDP not found\n");
        goto done;
    }
    if (wasmos_sync_user_read((int32_t)(uintptr_t)&rsdp_len,
                               (int32_t)sizeof(rsdp_len)) != 0 ||
        wasmos_sync_user_read((int32_t)(uintptr_t)&rsdp,
                               (int32_t)sizeof(acpi_rsdp_t)) != 0) {
        (void)printf("[acpi-bus] RSDP sync failed\n");
        goto done;
    }

    /* --- Step 2: locate DSDT via RSDT/XSDT -> FACP ------------------------ */
    int use_xsdt = (rsdp.revision >= 2 && rsdp.xsdt_address != 0);
    uint64_t sdt_phys = use_xsdt ? rsdp.xsdt_address
                                  : (uint64_t)rsdp.rsdt_address;

    /* Map first page of RSDT/XSDT to read its header. */
    if (map_phys(sdt_phys & ~(uint64_t)0xFFF, 4096) != 0) {
        (void)printf("[acpi-bus] RSDT/XSDT map failed\n");
        goto done;
    }
    uint32_t page_off = (uint32_t)(sdt_phys & 0xFFF);
    const acpi_sdt_hdr_t *sdt_hdr = (const acpi_sdt_hdr_t *)(g_map_window + page_off);
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
        sdt_hdr = (const acpi_sdt_hdr_t *)(g_map_window + page_off);
    }

    /* Walk RSDT/XSDT entries to find FACP. */
    uint32_t entry_size = use_xsdt ? 8u : 4u;
    uint32_t n_entries = (sdt_len - 36u) / entry_size;
    const uint8_t *entries = (const uint8_t *)sdt_hdr + 36u;
    uint64_t facp_phys = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        uint64_t ep = 0;
        if (use_xsdt) {
            __builtin_memcpy(&ep, entries + i * 8u, 8u);
        } else {
            uint32_t ep32 = 0;
            __builtin_memcpy(&ep32, entries + i * 4u, 4u);
            ep = (uint64_t)ep32;
        }
        /* Peek at the table signature: map one page to check. */
        uint64_t ep_page = ep & ~(uint64_t)0xFFF;
        uint32_t ep_off  = (uint32_t)(ep & 0xFFF);
        if (map_phys(ep_page, 4096) != 0) {
            continue;
        }
        const acpi_sdt_hdr_t *hdr = (const acpi_sdt_hdr_t *)(g_map_window + ep_off);
        if (hdr->signature[0] == 'F' && hdr->signature[1] == 'A' &&
            hdr->signature[2] == 'C' && hdr->signature[3] == 'P') {
            facp_phys = ep;
            break;
        }
    }
    if (facp_phys == 0) {
        (void)printf("[acpi-bus] FACP not found\n");
        goto done;
    }

    /* Map FACP to extract DSDT pointer. */
    uint64_t facp_page = facp_phys & ~(uint64_t)0xFFF;
    uint32_t facp_off  = (uint32_t)(facp_phys & 0xFFF);
    if (map_phys(facp_page, 4096) != 0) {
        (void)printf("[acpi-bus] FACP map failed\n");
        goto done;
    }
    const uint8_t *facp = g_map_window + facp_off;
    const acpi_sdt_hdr_t *facp_hdr = (const acpi_sdt_hdr_t *)facp;
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
    uint32_t dsdt_off  = (uint32_t)(dsdt_phys & 0xFFF);
    if (map_phys(dsdt_page, 4096) != 0) {
        (void)printf("[acpi-bus] DSDT map failed\n");
        goto done;
    }
    const acpi_sdt_hdr_t *dsdt_hdr = (const acpi_sdt_hdr_t *)(g_map_window + dsdt_off);
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
    const uint8_t *aml = g_map_window + dsdt_off + 36u;
    uint32_t aml_len = dsdt_len - 36u;
    scan_isa_devices(aml, aml_len, devmgr_ep, src_ep, &req_id);

done:
    (void)wasmos_ipc_send(devmgr_ep, src_ep,
                          DEVMGR_ACPI_SCAN_DONE, req_id,
                          0, 0, 0, 0);
    wasmos_sys_notify_ready(proc_endpoint, src_ep);
    return 0;
}
