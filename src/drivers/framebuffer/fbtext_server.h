#ifndef FBTEXT_SERVER_H
#define FBTEXT_SERVER_H

#include <stdint.h>
#include "wasmos_native_driver.h"

/*
 * The FBTEXT server every framebuffer driver variant runs (fbtext_server.c).
 *
 * The server maps the boot framebuffer the bootloader captured, builds the cell
 * grid, replays the kernel early log, registers as the console framebuffer, and
 * serves FBTEXT IPC (abi/opcodes.yaml, subsystem `fbtext`). What a variant adds
 * is how it reprograms its adapter, if it can at all.
 *
 * Runtime modes are the server's fixed mode list filtered to those whose
 * framebuffer (stride x height x 4 bytes, stride = width) fits the framebuffer
 * the firmware reported: the kernel maps the physical range captured at boot,
 * and no variant can enlarge it, so every mode reuses that memory. The range
 * may exceed what the boot mode uses (edk2's ramfb sizes it for 1024x768).
 */

typedef struct {
    /* Log prefix of the variant's probe line, e.g. "[framebuffer-pci]". */
    const char* tag;
    /* Called once with the boot geometry, after the framebuffer is mapped and
     * before the endpoint is registered. Returns 0 to serve, or a packed
     * WASMOS_ERR_* code that fbtext_serve returns as the driver's failure.
     * NULL for a variant with nothing to set up. */
    int32_t (*init)(wasmos_driver_api_t* api, const nd_framebuffer_info_t* boot);
    /* Programs the adapter to width x height at 32 bpp with `stride` pixels per
     * scanline, scanning out of the boot framebuffer. Returns 0, or a packed
     * WASMOS_ERR_GFX_* code sent back as the SET_RESOLUTION error. NULL for a
     * variant without runtime mode setting: it then answers QUERY_CAPS with no
     * capability and the mode opcodes with WASMOS_ERR_GFX_NO_RUNTIME_MODES. */
    int32_t (*set_mode)(wasmos_driver_api_t* api, uint32_t width, uint32_t height, uint32_t stride);
} fbtext_backend_t;

/* Runs the server for `backend`; `api` is the driver's entry argument. Returns
 * -2 on a native ABI mismatch, 0 when the machine has no usable framebuffer (a
 * headless boot, not an error), or a negative code when bring-up fails. On
 * success it does not return. */
int fbtext_serve(wasmos_driver_api_t* api, const fbtext_backend_t* backend);

#endif
