/* inttypes.h - PRIx32/PRId32 format-macro shims for freestanding kernel builds. */
#ifndef WASMOS_INTTYPES_H
#define WASMOS_INTTYPES_H

#include <stdint.h>

/* printf length/conversion fragments for the fixed-width types, defined only when the
 * toolchain has not already supplied them.  The kernel's own vsnprintf understands the
 * "ll" the 64-bit forms expand to, along with the conversions used here; it has no
 * floating-point or precision support, so no PRI macro beyond these is meaningful. */
#ifndef PRIi32
#define PRIi32 "d"
#endif
#ifndef PRIu32
#define PRIu32 "u"
#endif
#ifndef PRIx32
#define PRIx32 "x"
#endif
#ifndef PRIi64
#define PRIi64 "lld"
#endif
#ifndef PRIu64
#define PRIu64 "llu"
#endif
#ifndef PRIx64
#define PRIx64 "llx"
#endif

#endif
