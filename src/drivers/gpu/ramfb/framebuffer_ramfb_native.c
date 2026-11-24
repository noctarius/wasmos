#include "wasmos_native_driver.h"
#include "../../include/wasmos_driver_abi.h"
#include "fbtext_server.h"

/*
 * Native framebuffer driver — virtual terminal text plane, QEMU ramfb backing.
 *
 * QEMU's ramfb scans out of ordinary guest RAM. The firmware allocates that
 * buffer, reports it as the UEFI GOP framebuffer, and names it to QEMU by
 * writing the fw_cfg file "etc/ramfb"; this driver runs the shared FBTEXT server
 * (fbtext_server.c) on it and changes the mode by writing that file again, with
 * the same buffer and a new geometry. The modes offered are those that fit the
 * boot framebuffer (fbtext_server.h).
 *
 * device-manager binds it to the fw_cfg device (ACPI _HID QEMU0002), granting
 * fw_cfg's register window as region 0, and gives it a one-page DMA budget.
 * QEMU accepts fw_cfg file writes only through the DMA interface (writes to the
 * data register are ignored since QEMU 2.4), so every access below goes through
 * a DMA descriptor in that page. Protocol: QEMU docs/specs/fw_cfg.rst; ramfb
 * configuration layout: QEMU hw/display/ramfb.c.
 */

/* fw_cfg memory-mapped registers (QEMU virt). The DMA address register is a
 * big-endian 64-bit value; writing its low half starts the transfer, and the
 * transfer has completed by the time that write returns. */
#define FW_CFG_DMA_HI 0x10u
#define FW_CFG_DMA_LO 0x14u
/* Reading the DMA address register returns "QEMU CFG" (big-endian) when the DMA
 * interface is present; the first four bytes read as this little-endian word. */
#define FW_CFG_DMA_SIGNATURE_LO 0x554D4551u /* "QEMU" */

#define FW_CFG_FILE_DIR 0x0019u

/* FWCfgDmaAccess.control: a selector in the top 16 bits, operation bits below.
 * The device clears the operation bits on success and leaves ERROR set on
 * failure. */
#define FW_CFG_DMA_CTL_READ 0x02u
#define FW_CFG_DMA_CTL_SELECT 0x08u
#define FW_CFG_DMA_CTL_WRITE 0x10u

/* DRM_FORMAT_XRGB8888 ('X','R','2','4'), the 32 bpp layout the text plane draws. */
#define RAMFB_FOURCC_XRGB8888 0x34325258u

/* Layout of the DMA page: the descriptor, then the data it moves. */
#define DMA_DATA_OFFSET 64u

typedef struct __attribute__((packed)) {
    uint32_t control; /* big-endian */
    uint32_t length;  /* big-endian */
    uint64_t address; /* big-endian */
} fw_cfg_dma_access_t;

typedef struct __attribute__((packed)) {
    uint32_t size;   /* big-endian */
    uint16_t select; /* big-endian */
    uint16_t reserved;
    char name[56];
} fw_cfg_file_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;   /* big-endian */
    uint32_t fourcc; /* big-endian */
    uint32_t flags;  /* big-endian */
    uint32_t width;  /* big-endian */
    uint32_t height; /* big-endian */
    uint32_t stride; /* big-endian, bytes per scanline */
} ramfb_cfg_t;

static uint8_t* g_dma_page;
static uint64_t g_dma_phys;
static uint16_t g_ramfb_select;
static uint64_t g_fb_phys;

static uint32_t be32(uint32_t v) {
    return __builtin_bswap32(v);
}

static uint64_t be64(uint64_t v) {
    return __builtin_bswap64(v);
}

static void write_str(wasmos_driver_api_t* api, const char* s) {
    int n = 0;
    while (s[n]) {
        ++n;
    }
    api->console_write(s, n);
}

/* Runs one fw_cfg DMA transfer of `length` bytes between the data area of the
 * DMA page and the item `control` selects. Returns 0, or
 * WASMOS_ERR_DRIVER_DEVICE_INIT when the register write is refused or the
 * device reports an error. */
static int32_t fw_cfg_dma(wasmos_driver_api_t* api, uint32_t control, uint32_t length) {
    volatile fw_cfg_dma_access_t* access = (volatile fw_cfg_dma_access_t*)g_dma_page;
    uint64_t addr = g_dma_phys;
    access->control = be32(control);
    access->length = be32(length);
    access->address = be64(g_dma_phys + DMA_DATA_OFFSET);
    /* The register takes big-endian bytes; io_region_write stores in CPU
     * (little-endian) order, so each half is swapped. Its barriers order the
     * descriptor stores above before the doorbell. */
    if (api->io_region_write(0, FW_CFG_DMA_HI, 4, be32((uint32_t)(addr >> 32))) != 0 ||
        api->io_region_write(0, FW_CFG_DMA_LO, 4, be32((uint32_t)addr)) != 0) {
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    /* Success clears every control bit; ERROR, or any operation bit still set,
     * is a failed or unfinished transfer. */
    return be32(access->control) == 0 ? 0 : WASMOS_ERR_DRIVER_DEVICE_INIT;
}

static int names_equal(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Finds etc/ramfb in the fw_cfg file directory and records its selector.
 * Returns 0, or WASMOS_ERR_DRIVER_DEVICE_INIT when the directory cannot be read
 * or lists no ramfb (QEMU started without -device ramfb). */
static int32_t find_ramfb(wasmos_driver_api_t* api) {
    uint8_t* data = g_dma_page + DMA_DATA_OFFSET;
    uint32_t count_be = 0;
    if (fw_cfg_dma(api,
                   ((uint32_t)FW_CFG_FILE_DIR << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ,
                   4u) != 0) {
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    __builtin_memcpy(&count_be, data, 4u);
    uint32_t count = be32(count_be);
    /* The directory is read one entry at a time: each read continues where the
     * previous one stopped. */
    for (uint32_t i = 0; i < count; ++i) {
        fw_cfg_file_t file;
        if (fw_cfg_dma(api, FW_CFG_DMA_CTL_READ, (uint32_t)sizeof(file)) != 0) {
            return WASMOS_ERR_DRIVER_DEVICE_INIT;
        }
        __builtin_memcpy(&file, data, sizeof(file));
        file.name[sizeof(file.name) - 1] = '\0';
        if (names_equal(file.name, "etc/ramfb")) {
            g_ramfb_select = (uint16_t)__builtin_bswap16(file.select);
            return 0;
        }
    }
    return WASMOS_ERR_DRIVER_DEVICE_INIT;
}

/* Brings up the fw_cfg DMA path and locates etc/ramfb. A machine whose fw_cfg
 * has no DMA interface or no ramfb is declined: fw_cfg is on every QEMU virt
 * machine, and the framebuffer is then some other adapter's (the loader's
 * scanout for virtio-gpu, say). */
static int32_t ramfb_init(wasmos_driver_api_t* api, const nd_framebuffer_info_t* boot) {
    uint32_t signature = 0;
    void* page = 0;
    if (!api->io_region_read || !api->io_region_write || !api->dma_region_alloc) {
        return WASMOS_ERR_DRIVER_ABI_MISMATCH;
    }
    if (api->io_region_read(0, FW_CFG_DMA_HI, 4, &signature) != 0 ||
        signature != FW_CFG_DMA_SIGNATURE_LO) {
        write_str(api, "[framebuffer-ramfb] fw_cfg has no DMA interface\n");
        return FBTEXT_BACKEND_DECLINED;
    }
    int32_t rc = api->dma_region_alloc(1, &page, &g_dma_phys);
    if (rc != 0) {
        write_str(api, "[framebuffer-ramfb] no DMA page\n");
        return rc;
    }
    g_dma_page = (uint8_t*)page;
    if (find_ramfb(api) != 0) {
        write_str(api, "[framebuffer-ramfb] fw_cfg lists no etc/ramfb\n");
        return FBTEXT_BACKEND_DECLINED;
    }
    g_fb_phys = boot->framebuffer_base;
    write_str(api, "[framebuffer-ramfb] etc/ramfb found\n");
    return 0;
}

/* Rewrites etc/ramfb with the boot framebuffer's address and the new geometry;
 * QEMU resizes its scanout to it. */
static int32_t ramfb_set_mode(wasmos_driver_api_t* api, uint32_t width, uint32_t height,
                              uint32_t stride) {
    ramfb_cfg_t cfg;
    cfg.addr = be64(g_fb_phys);
    cfg.fourcc = be32(RAMFB_FOURCC_XRGB8888);
    cfg.flags = 0;
    cfg.width = be32(width);
    cfg.height = be32(height);
    cfg.stride = be32(stride * 4u);
    __builtin_memcpy(g_dma_page + DMA_DATA_OFFSET, &cfg, sizeof(cfg));
    if (fw_cfg_dma(api,
                   ((uint32_t)g_ramfb_select << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE,
                   (uint32_t)sizeof(cfg)) != 0) {
        return WASMOS_ERR_GFX_MODE_SET_FAILED;
    }
    return 0;
}

static const fbtext_backend_t g_backend = {"[framebuffer-ramfb]", ramfb_init, ramfb_set_mode};

/* Native driver entry point (see native_driver_entry_fn_t in
 * wasmos_native_driver.h); the return contract is fbtext_serve's. */
int initialize(wasmos_driver_api_t* api) {
    return fbtext_serve(api, &g_backend);
}
