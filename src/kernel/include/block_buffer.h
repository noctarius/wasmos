#ifndef WASMOS_KERNEL_BLOCK_BUFFER_H
#define WASMOS_KERNEL_BLOCK_BUFFER_H

#include <stdint.h>

#include "wasmos_status.h"

/*
 * block_buffer.h — bounds arithmetic for the per-process block bounce buffer.
 *
 * Both runtimes expose block_buffer_copy/write, both take (offset, len) straight
 * from the guest, and both must confine the resulting access to the process's
 * own buffer. The arithmetic lives here so there is one copy to get right; what
 * stays in each runtime is only what genuinely differs, namely how a slot is
 * found (a fixed array in wasm3, a hashmap in WARP).
 *
 * The whole test runs in 64 bits with an explicit overflow guard on offset+len.
 * Doing it in 32-bit unsigned arithmetic would let offset=0xFFFFFFFF, len=1 wrap
 * to 0 and pass, which is a guest-chosen out-of-bounds kernel read or write of
 * up to 4 GiB from the buffer base.
 *
 * Callers pass the guest's 32-bit values zero-extended. A negative int32 length
 * therefore arrives as a large positive one and is refused by the same bound,
 * which is why no separate sign test is needed.
 *
 * Returns WASMOS_OK when [offset, offset+len) lies within buf_bytes, else
 * WASMOS_ERR_BLOCK_RANGE. A zero length passes as long as offset itself is
 * within the buffer, matching the rest of the transfer surface.
 */
wasmos_error_code_t block_buffer_check_range(uint64_t offset, uint64_t len, uint64_t buf_bytes);

/*
 * Whether the buffer's physical address can be handed to the guest.
 *
 * block_buffer_phys returns the address as its SUCCESS value on the same i32
 * that carries the error codes, so an address with bit 31 set comes back as a
 * negative number and is read as an error. The bound is therefore 2 GiB, not
 * the 4 GiB the allocation asks for on DMA grounds -- an allocation satisfied
 * between 2 and 4 GiB is a legal DMA address that cannot be expressed in the
 * ABI's return value.
 *
 * Allocating below the tighter bound in the first place is what actually keeps
 * this from firing; the check is here so that a pool change cannot silently
 * turn an address into an error code.
 *
 * Returns WASMOS_OK for a nonzero address below BLOCK_BUFFER_PHYS_LIMIT, else
 * WASMOS_ERR_BLOCK_ABOVE_4G -- including for phys == 0, where the code names the
 * bound rather than the failure.
 */
wasmos_error_code_t block_buffer_check_phys(uint64_t phys);

/* The ceiling block_buffer_phys allocates under, for the reason above. */
#define BLOCK_BUFFER_PHYS_LIMIT 0x80000000ULL

#endif /* WASMOS_KERNEL_BLOCK_BUFFER_H */
