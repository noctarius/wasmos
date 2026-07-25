/* Minimal freestanding <inttypes.h> for the net-stack mbedTLS build.
 *
 * mbedTLS's debug.h includes <inttypes.h> for the PRI* printf format macros.
 * The kernel libc has none and clang's freestanding <inttypes.h> just
 * include_next's a nonexistent system header. This provides the fixed-width
 * PRI* macros for the LP64 x86_64-elf target (int=32, long=64, pointer=64).
 * On this include path for the mbedTLS and altcp_tls glue TUs only. */
#ifndef WASMOS_NET_STACK_INTTYPES_SHIM_H
#define WASMOS_NET_STACK_INTTYPES_SHIM_H

#include <stdint.h>

#define __PRI8 "hh"
#define __PRI16 "h"
#define __PRI32 ""
#define __PRI64 "l"
#define __PRIPTR "l"

#define PRId8 __PRI8 "d"
#define PRIi8 __PRI8 "i"
#define PRIo8 __PRI8 "o"
#define PRIu8 __PRI8 "u"
#define PRIx8 __PRI8 "x"
#define PRIX8 __PRI8 "X"

#define PRId16 __PRI16 "d"
#define PRIi16 __PRI16 "i"
#define PRIo16 __PRI16 "o"
#define PRIu16 __PRI16 "u"
#define PRIx16 __PRI16 "x"
#define PRIX16 __PRI16 "X"

#define PRId32 __PRI32 "d"
#define PRIi32 __PRI32 "i"
#define PRIo32 __PRI32 "o"
#define PRIu32 __PRI32 "u"
#define PRIx32 __PRI32 "x"
#define PRIX32 __PRI32 "X"

#define PRId64 __PRI64 "d"
#define PRIi64 __PRI64 "i"
#define PRIo64 __PRI64 "o"
#define PRIu64 __PRI64 "u"
#define PRIx64 __PRI64 "x"
#define PRIX64 __PRI64 "X"

#define PRIdPTR __PRIPTR "d"
#define PRIiPTR __PRIPTR "i"
#define PRIoPTR __PRIPTR "o"
#define PRIuPTR __PRIPTR "u"
#define PRIxPTR __PRIPTR "x"
#define PRIXPTR __PRIPTR "X"

#define PRIdMAX __PRI64 "d"
#define PRIiMAX __PRI64 "i"
#define PRIoMAX __PRI64 "o"
#define PRIuMAX __PRI64 "u"
#define PRIxMAX __PRI64 "x"
#define PRIXMAX __PRI64 "X"

#endif /* WASMOS_NET_STACK_INTTYPES_SHIM_H */
