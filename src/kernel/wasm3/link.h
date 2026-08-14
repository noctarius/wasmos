/* link.h - wasm3 hostcall registration declarations.
 * wasm3_link_all() registers every WASMOS import into a wasm3 runtime before
 * module instantiation; called once per wasm_driver_t startup. */
#ifndef WASMOS_WASM3_LINK_H
#define WASMOS_WASM3_LINK_H

#include "boot.h"
#include "wasm3.h"

/* Publish `boot_info` to the host calls that read it (ACPI RSDP, boot modules, boot
 * config, initfs) and initialise the per-pid side tables.  The pointer is borrowed and
 * must outlive every WASM process; the kernel passes its permanent boot_info.  Call
 * once, before any module is linked. */
void wasm3_link_init(const boot_info_t* boot_info);

/* Link every host call of the "wasmos" import module into `module`.  Returns 0 when
 * every import the module actually declares was satisfied, -1 for a NULL module or any
 * link failure (which is also logged).  An import the module does not declare is not a
 * failure: wasm3 reports m3Err_functionLookupFailed for it and that result is ignored.
 * wasm3 registers no wasi_snapshot_preview1 imports — those IDL entries are WARP-only. */
int wasm3_link_wasmos(IM3Module module);

/* Link the "env" import module: `strlen` (wasm3-only, no WARP counterpart) and the
 * AssemblyScript `abort`.  Same convention as wasm3_link_wasmos: 0 on success, -1 for a
 * NULL module or a link failure. */
int wasm3_link_env(IM3Module module);

#endif
