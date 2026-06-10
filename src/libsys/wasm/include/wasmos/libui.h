#ifndef WASMOS_LIBUI_H
#define WASMOS_LIBUI_H

/* Thin forwarder to the common src/libui single source of truth.
 * This removes the prior duplication between libc and libsys/wasm trees.
 * The authoritative implementation (including MENU_BAR/MENU_ITEM and full
 * component-owned render/layout/event handlers) now lives in one place.
 */
#include "../../../../libui/include/wasmos/libui.h"

#endif /* WASMOS_LIBUI_H */
