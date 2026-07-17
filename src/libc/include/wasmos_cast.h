/* wasmos_cast.h - readable spellings for the integer<->pointer reinterpret
 * casts that recur across the tree. Both go through uintptr_t so the cast is
 * size-clean (no -Wint-to-pointer-cast).
 *
 *   (uint64_t *)(uintptr_t)addr   ->  ptr_cast(uint64_t, addr)   // int -> data ptr
 *   (int32_t)(uintptr_t)ptr       ->  addr_cast(int32_t, ptr)    // ptr -> int
 *   (entry_fn_t)(uintptr_t)addr   ->  fn_cast(entry_fn_t, addr)  // int -> fn ptr
 *
 * ptr_cast's `type` is the pointee and the macro appends the `*`
 * (ptr_cast(uint64_t, x) yields uint64_t *). addr_cast and fn_cast take the
 * full target type verbatim. fn_cast exists so an address-to-function-pointer
 * cast reads by intent, and because ptr_cast's implicit `*` would wrongly
 * yield a pointer-to-function-pointer for an already-pointer typedef.
 */
#ifndef WASMOS_CAST_H
#define WASMOS_CAST_H

/* Force-included into every compile (see CFLAGS_KERNEL et al.), which includes
 * .S assembly units, so keep the body out of the assembler's way. */
#ifndef __ASSEMBLER__

#include <stdint.h>

#define ptr_cast(type, addr) ((type*)(uintptr_t)(addr))
#define addr_cast(type, ptr) ((type)(uintptr_t)(ptr))
#define fn_cast(type, addr) ((type)(uintptr_t)(addr))

#endif /* __ASSEMBLER__ */
#endif /* WASMOS_CAST_H */
