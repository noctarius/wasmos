/* startup.h - access to the startup arguments passed by PM at process launch */
#ifndef WASMOS_LIBC_WASMOS_STARTUP_H
#define WASMOS_LIBC_WASMOS_STARTUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy accessor. index 0 returns the process-manager endpoint (proc.endpoint),
 * sourced from the spawn-info contract; indices 1..3 are retired and return 0.
 * New code should prefer the named accessors below. */
int32_t wasmos_startup_arg(uint32_t index);

/* Named startup values, read from this process's wasmos_spawn_info_t at launch
 * (see wasmos_spawn_info.h). Zero when not provided. */
int32_t wasmos_startup_proc_endpoint(void);
int32_t wasmos_startup_tty(void);
uint32_t wasmos_startup_module_count(void);
uint32_t wasmos_startup_module_index(void);

/* Copy the NUL-terminated argv blob into dst (cap includes the NUL). Returns the
 * number of bytes written excluding the NUL. */
uint32_t wasmos_startup_args(char* dst, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif
