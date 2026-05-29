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

/* ---- AML pattern scanner for PNP0501 (16550-compatible serial) ----------- */

/* EISAID("PNP0501") encodes to bytes {0x41, 0xD0, 0x05, 0x01}. */
static const uint8_t PNP0501[4] = { 0x41, 0xD0, 0x05, 0x01 };

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

static void
scan_pnp0501(const uint8_t *aml, uint32_t aml_len,
             int32_t devmgr_ep, int32_t src_ep, int32_t *req_id)
{
    for (uint32_t i = 0; i + 5u <= aml_len; i++) {
        /* Match DWordPrefix (0x0C) + EISAID bytes for PNP0501. */
        if (aml[i] != 0x0Cu) {
            continue;
        }
        if (aml[i+1] != PNP0501[0] || aml[i+2] != PNP0501[1] ||
            aml[i+3] != PNP0501[2] || aml[i+4] != PNP0501[3]) {
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
                /* IO Port descriptor: 1 tag + 7 data bytes = 8 total.
                 * r += 7 so that the for-loop's r++ lands on the next tag. */
                io_base = (uint16_t)aml[pos+r+2] |
                          ((uint16_t)aml[pos+r+3] << 8);
                r += 7u;
                continue;
            }
            if ((tag == 0x22u || tag == 0x23u) && r + 2u < res_end) {
                /* Small IRQ descriptor: 0x22 = 3 bytes, 0x23 = 4 bytes.
                 * r += N-1 so that for-loop's r++ lands on the next tag. */
                uint16_t mask = (uint16_t)aml[pos+r+1] |
                                ((uint16_t)aml[pos+r+2] << 8);
                irq = irq_from_mask(mask);
                r += (tag == 0x23u) ? 3u : 2u;
                continue;
            }
        }

        if (io_base == 0 || irq == 0xFFu) {
            continue;
        }

        (void)printf("[acpi-bus] PNP0501 io=0x%04X irq=%u\n",
                     (unsigned)io_base, (unsigned)irq);

        /* Publish as non-PCI (bus=0xFF) device, class 0x07 (serial).
         * device_id field carries the ISA I/O base address. */
        uint32_t a0 = (0xFFu << 24) | 0x07u;
        uint32_t a1 = (0x02u << 16);               /* prog_if=0x02 (16550) */
        uint32_t a2 = (uint32_t)io_base;
        uint32_t a3 = ((uint32_t)irq << 8);
        (void)wasmos_ipc_send(devmgr_ep, src_ep,
                              DEVMGR_PUBLISH_DEVICE, (*req_id)++,
                              (int32_t)a0, (int32_t)a1,
                              (int32_t)a2, (int32_t)a3);
    }
}

/* ---- Main entry point ---------------------------------------------------- */

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
    scan_pnp0501(aml, aml_len, devmgr_ep, src_ep, &req_id);

done:
    (void)wasmos_ipc_send(devmgr_ep, src_ep,
                          DEVMGR_ACPI_SCAN_DONE, req_id,
                          0, 0, 0, 0);
    return 0;
}
