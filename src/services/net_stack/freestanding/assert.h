/* Freestanding <assert.h> shim for the net-stack mbedTLS build.
 *
 * The kernel libc has no <assert.h>, and mbedTLS's common.h includes it
 * unconditionally. mbedTLS uses assert() only for internal invariants; in this
 * freestanding ring-0 service a failed invariant cannot usefully abort, so
 * assert() is a no-op (as under NDEBUG). static_assert maps to the C11 keyword.
 * Placed on an include path ahead of the toolchain's <assert.h> for the mbedTLS
 * and altcp_tls glue translation units only. */
#ifndef WASMOS_NET_STACK_ASSERT_SHIM_H
#define WASMOS_NET_STACK_ASSERT_SHIM_H

#undef assert
#define assert(expr) ((void)0)

#ifndef static_assert
#define static_assert _Static_assert
#endif

#endif /* WASMOS_NET_STACK_ASSERT_SHIM_H */
