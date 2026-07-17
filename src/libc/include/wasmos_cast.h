/* wasmos_cast.h - readable spellings for the integer<->pointer reinterpret
 * casts that recur across the tree. Both go through uintptr_t so the cast is
 * size-clean (no -Wint-to-pointer-cast).
 *
 *   (uint64_t *)(uintptr_t)addr   ->  ptr_cast(uint64_t, addr)   // int -> ptr
 *   (int32_t)(uintptr_t)ptr       ->  addr_cast(int32_t, ptr)    // ptr -> int
 *
 * `type` for ptr_cast is the pointee (ptr_cast(uint64_t, x) yields uint64_t *).
 */
#ifndef WASMOS_CAST_H
#define WASMOS_CAST_H

/* Force-included into every compile (see CFLAGS_KERNEL et al.), which includes
 * .S assembly units, so keep the body out of the assembler's way. */
#ifndef __ASSEMBLER__

#include <stdint.h>

#define ptr_cast(type, addr) ((type *)(uintptr_t)(addr))
#define addr_cast(type, ptr) ((type)(uintptr_t)(ptr))

#endif /* __ASSEMBLER__ */
#endif /* WASMOS_CAST_H */
