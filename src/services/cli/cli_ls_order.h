/* cli_ls_order.h - ordering for `ls` listings.
 *
 * Split out of cli.c for the same reason fs_manager_path.c is split out of
 * fs_manager.c: these two functions touch no IPC, no console and no global
 * state, so they can be unit-tested directly.
 *
 * Ordering belongs here rather than in the filesystem. FAT specifies none and
 * POSIX readdir() guarantees none, and the backend streams entries as it walks
 * the cluster chain -- sorting there would mean buffering a whole directory
 * before emitting the first byte. */
#ifndef WASMOS_CLI_LS_ORDER_H
#define WASMOS_CLI_LS_ORDER_H

#include <stdint.h>

/* Compare two listing entries the way a person reads them: case-insensitively,
 * and with digit runs compared by VALUE so "f9" precedes "f10" (lexicographic
 * order would put "f10" first). A single trailing '/' marks a directory and is
 * not part of the name, so "ab/" and "ab" compare equal.
 *
 * Returns <0, 0 or >0 like strcmp. */
int cli_ls_name_cmp(const char* a, const char* b);

/* Sort `count` offsets into `pool` by the names they point at, in place.
 * Insertion sort: the caller bounds `count`, and this libc has no qsort.
 * Stable, so equal names keep their arrival order. */
void cli_ls_sort(const char* pool, uint16_t* offsets, uint32_t count);

#endif /* WASMOS_CLI_LS_ORDER_H */
