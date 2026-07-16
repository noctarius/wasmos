/* arch/cc.h - lwIP compiler/platform abstraction for the wasmos native
 * net-stack service.
 *
 * Freestanding x86_64 build. Integer types come from the compiler's <stdint.h>
 * (available in freestanding mode). Unlike the earlier kernel-image variant,
 * this is a native service (.wap) and MUST NOT depend on kernel headers such as
 * <klog.h>. Diagnostics are routed to a small port helper implemented in
 * net_stack.c (which has the wasmos_driver_api_t console hook).
 */
#ifndef WASMOS_NET_STACK_ARCH_CC_H
#define WASMOS_NET_STACK_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* --- Byte order ---------------------------------------------------------- */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* --- No host <inttypes.h> ------------------------------------------------ */
/* This is a freestanding build; pulling in the host <inttypes.h> drags in
 * glibc's <bits/wordsize.h> which does not exist for the kernel target. Tell
 * lwIP not to include it and provide the (sn)printf format macros ourselves.
 * Values assume 32-bit int / ILP: X8_F etc. per lwip/arch.h contract. */
#define LWIP_NO_INTTYPES_H 1
#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

/* --- Struct packing ------------------------------------------------------ */
/* GCC/clang style: add the packed attribute at the end via PACK_STRUCT_STRUCT. */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x

/* --- Diagnostics --------------------------------------------------------- */
/* net_stack.c provides a printf-style logger over the native console hook.
 * Kept minimal for this compile milestone. */
void net_stack_lwip_diag(const char* fmt, ...);

#define LWIP_PLATFORM_DIAG(x)                                                                      \
    do {                                                                                           \
        net_stack_lwip_diag x;                                                                     \
    } while (0)

#define LWIP_PLATFORM_ASSERT(x)                                                                    \
    do {                                                                                           \
        net_stack_lwip_diag("lwip assert: \"%s\" at %s:%d\n", (x), __FILE__, __LINE__);            \
        for (;;) {                                                                                 \
        }                                                                                          \
    } while (0)

#endif /* WASMOS_NET_STACK_ARCH_CC_H */
