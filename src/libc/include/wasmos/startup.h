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
 * number of bytes written excluding the NUL. The blob is captured once at first
 * access and truncated to 255 bytes there (WASMOS_STARTUP_ARGS_MAX in
 * spawn_info.c), so a longer command line is cut short for every caller. */
uint32_t wasmos_startup_args(char* dst, uint32_t cap);

/* Split the argument string into an argv array and return argc (>= 1), or 0 when
 * the arguments are unusable (NULL buf/argv, buf_cap 0, or argv_max below 2).
 * `buf` receives the tokens NUL-separated in place and must outlive argv;
 * argv_max counts argv[0] and the NULL terminator.
 *
 * argv[0] is an EMPTY program-name slot -- the contract carries no name -- so
 * argv[1] is the first argument, as in any C program. An argument that does not
 * fit `buf` whole is dropped rather than truncated. crt1 calls this to build
 * main()'s argc/argv; see src/libc/src/spawn_info.c for the full contract. */
int wasmos_startup_argv(char* buf, uint32_t buf_cap, char** argv, uint32_t argv_max);

#ifdef __cplusplus
}
#endif

#endif
