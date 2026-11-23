#include "wasmos_native_driver.h"
#include "fbtext_server.h"

/*
 * Native framebuffer driver — virtual terminal text plane, UEFI GOP backing.
 *
 * Runs the shared FBTEXT server (fbtext_server.c) on whatever mode the
 * bootloader left behind: this driver has no way to reprogram the adapter, so
 * it answers the mode-setting opcodes with WASMOS_ERR_GFX_NO_RUNTIME_MODES.
 * framebuffer_pci (Bochs/QEMU VBE) and framebuffer_ramfb (QEMU ramfb) are the
 * variants that can.
 */

static const fbtext_backend_t g_backend = {"[framebuffer]", 0, 0};

/* Native driver entry point (see native_driver_entry_fn_t in
 * wasmos_native_driver.h); the return contract is fbtext_serve's. */
int initialize(wasmos_driver_api_t* api) {
    return fbtext_serve(api, &g_backend);
}
