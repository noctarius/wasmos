#ifndef WASMOS_LIBUI_H
#define WASMOS_LIBUI_H

/* Thin forwarder to the common src/libui single source of truth.
 * This removes the prior duplication between libc and libsys/wasm trees.
 * All updates happen in src/libui/include/wasmos/libui.h (and its libui_*.h components).
 * Consumers continue to use #include "wasmos/libui.h" unchanged.
 */
#include "../../../libui/include/wasmos/libui.h"

#endif /* WASMOS_LIBUI_H */
