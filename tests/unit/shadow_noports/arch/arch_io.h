/* Host shadow of src/kernel/include/arch/arch_io.h for tests/unit/test_io_region.c:
 * selects the aarch64 accessors, the architecture without an I/O-port space,
 * whatever the host is. They are plain C, so they run on the host unchanged.
 * The memory-mapped accessors stay the real header's. Kept out of
 * tests/unit/include so no other suite picks it up in place of the real one. */
#pragma once

#include "arch/aarch64/arch_io.h"
