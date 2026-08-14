/* assert.h - Kernel assertion macro: calls kpanic on failure. */
#ifndef WASMOS_ASSERT_H
#define WASMOS_ASSERT_H

#include <stdlib.h>

/* Panic the machine when `cond` is false.  Unlike the C standard macro this is NOT
 * compiled out by NDEBUG — the condition is always evaluated, so an expression with side
 * effects behaves the same in every build.  The panic reports "abort" and neither the
 * condition text nor the source location, so a distinguishable kpanic call is a better
 * diagnostic where one is affordable. */
#define assert(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

#endif
