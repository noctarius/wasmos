/* wasmos_cast.h - readable spellings for the integer<->pointer reinterpret
 * casts that recur across the tree. Both go through uintptr_t so the cast is
 * size-clean (no -Wint-to-pointer-cast).
 *
 *   (uint64_t *)(uintptr_t)addr   ->  ptr_cast(uint64_t, addr)   // int -> ptr
 *   (int32_t)(uintptr_t)ptr       ->  addr_cast(int32_t, ptr)    // ptr -> int
 *
 * The difference is the `*`: ptr_cast's `type` is the pointee and the macro
 * adds the star (ptr_cast(uint64_t, x) yields uint64_t *), while addr_cast
 * takes the full target type verbatim. addr_cast is therefore the right form
 * whenever the target is already a pointer type spelled as one - e.g. casting
 * an address to a function-pointer typedef, where ptr_cast would wrongly yield
 * a pointer-to-function-pointer.
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
