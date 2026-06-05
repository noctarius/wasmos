# WASMOS Bug Report

**Round 1 inspection:** 2026-05-25 — ~97 bugs found, all critical/high/medium now fixed.  
**Round 2 inspection:** 2026-06-05 — 41 raw findings, ~26 confirmed after false-positive triage.

---

## Critical Bugs

### N-C-1 — `src/kernel/memory.c:724,729-730` — Double-free of physical pages in `mm_shared_unmap`

`mm_shared_unmap` calls `pfa_free_pages(region->base, region->pages)` unconditionally at line 724 to "unpin" the pages. It then decrements `region->refcount` (line 729) and calls `mm_shared_free_if_unused` (line 730), which calls `pfa_free_pages` a second time when `refcount` reaches 0. Any caller that holds the last reference triggers a double-free, corrupting the physical frame allocator.

---

## High Severity Bugs

### N-H-1 — `src/services/vt/vt_main.c:1517-1521` — VT_IPC_WRITE_REQ count field not clamped (OOB read)

Introduced by the M-21 fix. The byte count is extracted as `(args[0] >> 24) & 0xF`, which yields 0–15. The loop then iterates `count` times into the 4-element `args[]` array. A sender with count > 4 causes out-of-bounds stack reads. Fix: clamp count to 4 before the loop.

### N-H-2 — `src/drivers/framebuffer_pci/framebuffer_pci_native.c:246-265` — Missing ring overflow guard in `drain_console_ring`

This is the companion file to M-15. `framebuffer_native.c` received the producer-lap snap-forward fix but `framebuffer_pci_native.c` has an identical `drain_console_ring` with the same bug: no `if ((uint32_t)(wp - rp) > cap) rp = wp - cap;` guard before the drain loop.

### N-H-3 — `src/kernel/native_driver.c:748-749` — ELF segment offset overflow in native driver loader

`ph->p_offset + ph->p_filesz > (uint64_t)elf_size` can be bypassed when both operands are `uint64_t` and their sum wraps. A crafted ELF with `p_offset` near `UINT64_MAX` passes the check while the actual access goes far out of bounds. The safe form is `ph->p_filesz > elf_size - ph->p_offset` (subtraction first, same pattern as the M-12 fix in boot.c).

### N-H-4 — `src/drivers/fs_fat/fs_fat.c:2937` — Integer overflow in FAT file capacity extension

`file->capacity += (uint32_t)g_sectors_per_cluster * g_bytes_per_sector` can wrap when capacity approaches `UINT32_MAX`. The `while (file->capacity < min_size)` loop exits prematurely on wrap, leaving the file with insufficient allocated clusters. Subsequent writes corrupt adjacent data.

### N-H-5 — `src/services/device_manager/device_manager.c:1207` — Hardcoded IRQ mask in `query_driver_module_meta`

The function unconditionally sets `out_caps->irq_mask = (1u << 14) | (1u << 15)` regardless of what the module metadata actually declares. The IRQ mask should be decoded from the IPC response (arg3), like the other capability fields. All drivers currently receive IRQ 14/15 access and lose their actual declared IRQ requirements.

### N-H-6 — `src/services/pci_bus/pci_bus.c:143-144` — PCI multi-function check reads header type from function 0 instead of current function

`pci_config_read32(bus, device, 0, 0x0C)` always reads the header type from function 0. When scanning functions 1–7, the loop incorrectly breaks if function 0 reports single-function, skipping legitimately present higher functions. Should be `pci_config_read32(bus, device, function, 0x0C)`.

### N-H-7 — `src/services/gfx_compositor/gfx_compositor.zig:1530` — Integer overflow in glyph mask array index

`y * glyph_w + x` uses `i32` multiplication. With glyph dimensions up to 65535 (from packed `u16`), `y * glyph_w` overflows `i32` and the resulting index silently wraps when cast to `usize`, producing an out-of-bounds access in the mask buffer.

### N-H-8 — `src/services/font_service/font_service.zig:713-714` — Integer overflow in rasterisation buffer index

`gy * w + gx` and `py * out_w + px` use `i32` arithmetic. At large glyph dimensions (w or out_w up to 65535), the multiplication overflows and the index wraps, causing out-of-bounds writes to the scratch or destination buffer.

### N-H-9 — `src/libc/include/wasmos/libui.h:428,555,1207` — Integer overflow in capacity-doubling loops

Three sites perform `cap *= 2` (or `new_cap *= 2`) without overflow checks. When `cap` exceeds `INT32_MAX / 2`, doubling wraps to a small or negative value, causing a subsequent `malloc` to allocate an undersized buffer that is then written past.

### N-H-10 — `src/libc/src/unistd.c:221` — `read()` silently truncates `count` from `size_t` to `int32_t`

`wasmos_console_read((int32_t)(uintptr_t)buf, (int32_t)count)` — the cast of `count` to `int32_t` truncates any value > `INT32_MAX`, causing a partial read with no error indication.

### N-H-11 — `src/libc/src/unistd.c:277` — `write()` truncates count and returns wrong byte count

`wasmos_console_write` is called with `(int32_t)count` (truncated), but the function then returns the original `count` rather than the truncated `wrote` value, claiming more bytes were written than actually were.

### N-H-12 — `src/boot/boot.c:916-917` — Integer overflow in boot info allocation size

`total_bytes = boot_bytes + map_bytes + initfs_size + module_table_bytes` — all `UINTN`. On a large initfs, this addition can silently wrap, making `total_pages` smaller than required. The subsequent writes at lines 933, 954, 962, and 971 then overflow the under-allocated boot buffer, corrupting kernel memory.

### N-H-13 — `src/boot/boot.c:952-958` — Boot buffer cursor not bounds-checked after each layout step

After copying the initfs at line 954 and advancing `cursor += initfs_size` at line 958, no check verifies that the new cursor is still within the `total_pages * 4096` bytes allocated at line 919. If `initfs_size` is larger than expected (corrupted or attacker-controlled), subsequent `memset8` and module-table writes go out of bounds.

---

## Medium Severity Bugs

### N-M-1 — `src/kernel/process_manager_buffers.c:145-166` — Double-borrow clobbers active borrow state in `pm_fs_buffer_borrow_context`

`pm_fs_buffer_borrow_context` does not check whether `borrower->borrow_active` is already set before overwriting `borrow_source_context_id` and `borrow_flags`. A second concurrent borrow silently replaces the first, orphaning the original source context and breaking its release path.

### N-M-2 — `src/kernel/process_manager_buffers.c:438-447` — Framebuffer slot table iterated without lock in `process_manager_buffer_drop_context`

`g_pm_fb_slots` is walked and modified without holding any lock. A concurrent `pm_fb_slot_for_context` or `pm_fb_buffer_for_context` call on another CPU can race with the removal, corrupting the iteration or producing use-after-free.

### N-M-3 — `src/kernel/serial.c:210-212` — Missing NULL check in `serial_ring_init` after `mm_shared_create`

`*ring_slot = (console_ring_t *)(uintptr_t)phys_base` is assigned without checking whether `mm_shared_create` returned `phys_base = 0`. A NULL console ring pointer is then dereferenced on every subsequent kernel log write.

### N-M-4 — `src/kernel/wasm3_link.c:2109-2110` — Unsigned wraparound in `wasmos_shmem_map_auto` scan loop

`off64 + map_size <= mem_size` — if `off64` is near `UINT64_MAX` and `map_size` is added, the sum wraps to a small value, making the condition spuriously true. This can allow mapping at an invalid offset, bypassing the subsequent bounds check at lines 2114–2117.

### N-M-5 — `src/services/font_service/font_service.zig:691-692` — Overflow in pixel count validation uses signed wrapping as its error signal

`pixel_count_i32 = w * hgt` — both `i32`. For large positive dimensions, the product overflows to a negative value and the `<= 0` check catches it, but this relies on signed overflow being defined (it is in Zig), and means valid large glyphs are silently rejected. Widening to `i64` before the multiply would make the intent explicit.

### N-M-6 — `src/libc/include/wasmos/libui.h:348` — Integer overflow in glyph mask array index

`mask[gy * w + gx]` — `i32` multiplication. Same pattern as N-H-7/N-H-8: overflows for large glyph widths, producing an incorrect index and out-of-bounds access.

### N-M-7 — `src/libsys/wasm/include/wasmos/libsys.h:201` — Inconsistent `request_id` validation between WASM and native libsys

`wasmos_sys_intent_send_with_request_id` rejects `request_id <= 0`, while the native variant in `libsys_native.c:363` only rejects `request_id == 0`. Callers using negative request IDs (e.g. -1 as a sentinel) behave differently depending on whether they compiled against the WASM or native libsys, breaking the protocol contract.

---

## Low Severity Bugs

### N-L-1 — `src/kernel/process_manager_spawn.c:1496-1500` — `g_pm.spawn` state written without serialisation in `pm_handle_spawn_path`

The `g_pm.spawn.in_use` check at line 1498 is not atomic with the subsequent field writes at lines 1516–1525. On SMP, two concurrent spawn requests can both pass the check and race to set `spawn.is_sync`, `spawn.reply_endpoint`, etc., corrupting spawn state.

### N-L-2 — `src/kernel/paging.c:413-415,430` — Unconditional `pfa_free_pages` in error path may free address 0

In `paging_create_address_space`, if `IDENTITY_PD_COUNT == 0`, `child_pdpt_low` is never allocated. The error path at line 430 calls `pfa_free_pages(child_pdpt_low, 1)` unconditionally. `pfa_free_pages` guards against base==0, so no corruption occurs today, but the intent is wrong and fragile.

---

## Open Bug Summary (round 1 + round 2)

| Subsystem | Critical | High | Medium | Low | Total |
|---|---|---|---|---|---|
| Kernel (memory, paging, process, serial, wasm3_link) | 1 | 3 | 4 | 2 | 10 |
| Drivers (ata, mouse, serial, framebuffer, framebuffer_pci, keyboard) | 0 | 3 | 0 | 1 | 4 |
| Services (vt, fat, device_mgr, pci_bus, gfx_comp, font_service) | 0 | 4 | 1 | 0 | 5 |
| Libc/libsys (unistd, libui, libsys.h) | 0 | 5 | 2 | 0 | 7 |
| Boot (boot.c) | 0 | 2 | 0 | 0 | 2 |
| **Round 2 total** | **1** | **17** | **7** | **3** | **28** |
| **Round 1 carry-over (L-1..L-22, H-19)** | 0 | 1 | 0 | 22 | 23 |
| **Grand total** | **1** | **18** | **7** | **25** | **51** |

---

## Lock Hierarchy (authoritative reference)

```
g_pfa_lock  (outermost)
  → g_endpoint_table_lock
    → ep->lock
      → g_process_table_lock
        → g_ready_queue_lock
          → g_thread_table_lock  (innermost)
```

Never acquire an outer lock while holding an inner one. Full details: `docs/LOCK_HIERARCHY.md`.
