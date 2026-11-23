#include "wasmos_native_driver.h"
#include "../include/wasmos_driver_abi.h"
#include "fbtext_server.h"

/*
 * Native framebuffer driver — virtual terminal text plane, Bochs/QEMU VBE (BGA)
 * backing.
 *
 * Runs the shared FBTEXT server (fbtext_server.c) and adds runtime mode setting
 * through the BGA index/data ports, so it answers QUERY_CAPS with
 * SET_RESOLUTION | QUERY_MODES. The modes offered are those that fit the boot
 * framebuffer mapping (fbtext_server.h).
 */

/* Bochs/QEMU VBE extensions (VBE_DISPI): write a register index to the index
 * port, then its value to the data port. ENABLE must be cleared before the
 * geometry registers are changed and set again afterwards. */
#define BGA_INDEX_PORT 0x01CEu
#define BGA_DATA_PORT 0x01CFu
#define BGA_REG_XRES 0x0001u
#define BGA_REG_YRES 0x0002u
#define BGA_REG_BPP 0x0003u
#define BGA_REG_ENABLE 0x0004u
#define BGA_ENABLED 0x0001u
#define BGA_LFB_ENABLED 0x0040u

static void bga_write_reg(wasmos_driver_api_t* api, uint16_t reg, uint16_t value) {
    api->io_out16(BGA_INDEX_PORT, reg);
    api->io_out16(BGA_DATA_PORT, value);
}

/* BGA scans out with stride == width, which is the only stride the server
 * asks for. */
static int32_t bga_set_mode(wasmos_driver_api_t* api, uint32_t width, uint32_t height,
                            uint32_t stride) {
    (void)stride;
    if (!api->io_out16 || width > 0xFFFFu || height > 0xFFFFu) {
        return WASMOS_ERR_GFX_MODE_SET_FAILED;
    }
    bga_write_reg(api, BGA_REG_ENABLE, 0u);
    bga_write_reg(api, BGA_REG_XRES, (uint16_t)width);
    bga_write_reg(api, BGA_REG_YRES, (uint16_t)height);
    bga_write_reg(api, BGA_REG_BPP, 32u);
    bga_write_reg(api, BGA_REG_ENABLE, (uint16_t)(BGA_ENABLED | BGA_LFB_ENABLED));
    return 0;
}

static const fbtext_backend_t g_backend = {"[framebuffer-pci]", 0, bga_set_mode};

/* Native driver entry point (see native_driver_entry_fn_t in
 * wasmos_native_driver.h); the return contract is fbtext_serve's. */
int initialize(wasmos_driver_api_t* api) {
    return fbtext_serve(api, &g_backend);
}
