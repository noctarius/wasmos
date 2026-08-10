#ifndef WASMOS_KERNEL_KENV_H
#define WASMOS_KERNEL_KENV_H

#include <stdint.h>

#include "wasmos_status.h"

/*
 * kenv.h — the kernel environment store.
 *
 * One store, shared by both runtimes. It was previously written out once per
 * runtime, with WARP's copy carrying the comment "mirrors the static table in
 * wasm3/link.c", and the two mirrors drifted: env_set refused an over-long key
 * in both, while wasm3's env_get truncated it and looked up the prefix, so a
 * guest could read a variable it had not named and could not have created.
 *
 * What stays in each runtime is the guest-memory plumbing -- translating a wasm
 * pointer, checking the range is permitted, copying in and out. None of that
 * belongs to the store, and it is the only part that genuinely differs.
 *
 * The store holds no lock. Host calls run with the calling process descheduled
 * and the kernel does not touch the environment from interrupt context, which
 * matches how both hand-written copies behaved.
 */

#define KENV_MAX_ENTRIES 64
#define KENV_KEY_MAX 33  /* including the NUL */
#define KENV_VAL_MAX 129 /* including the NUL */

/*
 * Look up `key` and copy its value into `out`, always NUL-terminating.
 *
 * A value longer than `out_size` is truncated to fit and `*written` reports
 * what was copied, so a caller can tell truncation happened. A key at or over
 * KENV_KEY_MAX is REFUSED, never shortened: shortening resolves to whichever
 * variable shares the surviving prefix.
 */
wasmos_error_code_t kenv_get(const char* key, char* out, uint32_t out_size, uint32_t* written);

/* Create or replace `key`. Replacing consumes no additional entry, so it
 * succeeds even when the table is full. */
wasmos_error_code_t kenv_set(const char* key, const char* value);

/* Remove `key`. Removing something absent is not an error -- the caller's
 * intent is satisfied either way -- but an unusable key is still refused. */
wasmos_error_code_t kenv_unset(const char* key);

/* Entries in use. For tests and for kmap-style reporting. */
uint32_t kenv_count(void);

/* Drop every entry. Used by tests to isolate cases; the kernel never needs it. */
void kenv_reset(void);

#endif /* WASMOS_KERNEL_KENV_H */
