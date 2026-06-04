# WASMOS Deep Inspection Bug Report

**Date:** 2026-05-25  
**Scope:** Kernel, drivers, services, libc, apps — full source inspection  
**Agents:** 4 parallel inspection agents covering kernel, drivers/VT/font, gfx_compositor/services, and libc/apps  
**Total bugs found:** ~97 confirmed bugs (excluding false positives)

---

## Critical Bugs

These bugs cause data corruption, security regressions, or guaranteed crashes.

### C-1 ✅ FIXED — `src/libc/assemblyscript/wasmos.ts` and `src/libc/zig/wasmos.zig` — `IPC_FIELD_ARG2`/`IPC_FIELD_ARG3` constants have wrong field indices

```typescript
const IPC_FIELD_ARG2: i32 = 4;  // WRONG: field 4 is SOURCE
const IPC_FIELD_ARG3: i32 = 5;  // WRONG: field 5 is DESTINATION
```

The kernel ABI (`wasmos_driver_abi.h`, confirmed in `wasm3_link.c`) defines an 8-field layout:
fields 0–3 = TYPE, REQUEST_ID, ARG0, ARG1; field 4 = SOURCE; field 5 = DESTINATION; fields 6–7 = ARG2, ARG3.
Both the AssemblyScript and Zig WASM libc files defined `IPC_FIELD_ARG2 = 4` and `IPC_FIELD_ARG3 = 5`,
skipping SOURCE and DESTINATION entirely. Any WASM driver/service reading ARG2 or ARG3 from a message
actually received the sender's endpoint ID and destination endpoint instead of the payload bytes.
The C libc `ipc.h` was correct; only the WASM libc files were wrong. **Fixed.**

### C-2 ✅ FIXED — `src/libc/src/string.c:254-263` — `memmove` backward 32-byte block copies chunks in wrong order

```c
out -= 32;
in  -= 32;
copy8_backward(out,      in);       // copies bytes 0–7 first
copy8_backward(out + 8,  in + 8);   // then 8–15
copy8_backward(out + 16, in + 16);  // then 16–23
copy8_backward(out + 24, in + 24);  // then 24–31
```

A safe backward copy must write the highest bytes first. For any overlapping `memmove` where `dest > src` and `dest - src < 32` and `count >= 32`, the first `copy8_backward` clobbers source bytes that the later chunks still need to read. **Corrupts overlapping copies.**

### C-3 ✅ FIXED — `src/boot/boot.c:956-962` — Misleading indentation on for-loop closing brace (false positive — code is correct)

The `for`-loop closing `}` was indented at the same column as the surrounding `if (module_count > 0)`, making it appear that `boot_info->modules = mods` runs outside the `if` block. Brace counting proves otherwise: the `}` closes the `for` loop, and lines 959–961 are still inside the `if`. When `module_count == 0` the whole block is correctly skipped. **Fixed by correcting the indentation.** No behavioral change.

### C-4 ✅ FIXED — `src/kernel/ipc.c:75-87` — Endpoint slot claim has no lock (race condition)

`ipc_endpoint_create` scans for a free slot and claims it without holding a lock during the scan-and-set. Two concurrent callers can both see the same slot free and both claim it, silently aliasing two endpoints to one slot.

### C-5 ✅ FIXED — `src/kernel/paging.c` — NX bit dropped when splitting a 2 MB large page

When replacing a large-page PDE with a PT, the new 4 KB PTEs are built with `PT_FLAG_PRESENT | PT_FLAG_WRITE | table_flags` but without the `PT_FLAG_NX` bit from the original large-page entry. A formerly non-executable region becomes executable after the split — a privilege escalation vector.

### C-6 ✅ FIXED — `src/kernel/memory.c` — Wrong region gets `phys_base` overwritten in `mm_shared_map`

After `mm_context_add_region` inserts the shared region, the code walks to the "last" list element and assigns `last->phys_base`. With the array-chunk list implementation, `list_alloc` fills any freed slot — not necessarily the last-iterated one. An unrelated region can have its `phys_base` silently overwritten.

### C-7 ✅ FIXED — `src/kernel/arch/x86_64/irq_x86_64.c` — Spurious IRQ 7 sends EOI, corrupting PIC state

Per the Intel 8259A spec, a spurious IRQ 7 must NOT receive an EOI (the PIC never latched the interrupt, so its ISR bit is not set). Sending EOI clears an unrelated ISR bit, causing subsequent real IRQ 7 events to be lost or mishandled.

### C-8 ✅ FIXED — `src/kernel/arch/x86_64/cpu_x86_64.c` — TSS RSP0 and IST1 share the same stack

Both `g_tss.rsp0` and `g_tss.ist1` point to `ist1_top`. RSP0 is used for ring-3→ring-0 transitions; IST1 is for the timer IRQ. A timer interrupt during a privilege transition corrupts the transition frame and the interrupt frame simultaneously.

---

## High Severity Bugs

### H-1 ✅ FIXED — `src/kernel/memory.c` — `mm_shared_retain` refcount has no overflow guard

`region->refcount++` at `UINT32_MAX` wraps to 0. The next `mm_shared_release` frees the region while all other holders still reference it — use-after-free.

### H-2 ✅ FIXED — `src/kernel/memory.c` — `mm_context_alloc_region`: zombie entry on alloc failure

`mm_context_add_region_slot` inserts the region (increments `region_count`) before the `pfa_alloc_pages` check. When allocation fails and the function returns -1, the zero-base dead entry remains in the region list permanently.

### H-3 ✅ FIXED — `src/kernel/memory.c` — CPU left with user page table on restore failure in `mm_copy_from_user`

If `paging_switch_root(prev_root)` fails in `mm_copy_from_user_impl`, the function returns -1 while the CPU is still running under the user's page table. All subsequent kernel execution operates under user mappings.

### H-4 ✅ FIXED — `src/kernel/paging.c` — Leaf frame double-free in `paging_destroy_address_space` corrupts page tables

The original fix for this entry (commit b0bad301) added leaf-frame freeing to `paging_destroy_address_space`, but that is the wrong layer: `mm_context_release_regions` already frees the physical data pages as part of memory-context teardown. The double-free returned the same physical page to the allocator twice; a subsequent `alloc_table` call could allocate it as a new process's PML4, then `zero_page` would clear PML4[511], silently removing the kernel higher-half mapping. The next TLB flush on that CR3 caused a #PF → #DF → triple fault. Fixed by reverting the leaf-frame walk from `paging_destroy_address_space` so it only frees page-table structure pages (PT, PD, PDPT). Data-page lifetime is owned exclusively by `mm_context_release_regions`.

### H-5 ✅ FIXED — `src/kernel/process_manager_spawn.c` — Spawned process not killed on `pm_apply_spawn_caps` failure

When `pm_spawn_module` succeeds but `pm_apply_spawn_caps` subsequently fails, the new process runs without its intended capability profile and no caller has the PID to kill it.

### H-6 ✅ FIXED — `src/kernel/ipc.c:193-204` — `waiter_tid` set on non-blocking poll, blocking waiter never woken

Every time `ipc_recv_for` finds an empty queue it stores `ep->waiter_tid = tid`, including for non-blocking polls. A subsequent blocked waiter sets its own `waiter_tid`, then a message arrives and `ipc_send_from` wakes the earlier poller (already running). The blocked waiter is never woken.

### H-7 ✅ FIXED — `src/kernel/ipc.c:283-298` — TOCTOU in `ipc_endpoints_release_owner`

The release loop reads `ep->in_use` without the lock. A concurrent `ipc_endpoint_get` can pass the get-check, then the slot is zeroed, then the remote operates on cleared endpoint state — use-after-free.

### H-8 ✅ FIXED — `src/kernel/memory_service.c` — Page-fault reply fails on full IPC queue, killing resolvable faulting process

If the IPC reply queue is full, `ipc_send_from` returns `IPC_ERR_FULL`. This propagates as -1 and the kernel terminates the process, even though the fault was resolvable. No retry exists.

### H-9 ✅ FIXED — `src/kernel/native_driver.c` — Prior ELF segments leaked on partial load failure

When loading ELF segments, if segment N fails, segments 0..N-1 that were already mapped are not freed — permanent physical memory leak on any ELF load failure.

### H-10 ✅ FIXED — `src/kernel/native_driver.c` — Shared region orphaned on `mm_shared_retain` failure

`mm_shared_create` succeeds and the slot is live. If `mm_shared_retain` then fails, the slot is permanently occupied and the region is never freed (caller has no ID to release it).

### H-11 ✅ FIXED — `src/kernel/native_driver.c` — ELF entry point not validated before call

`hdr->e_entry` is cast directly to a function pointer with no check that the address falls within a loaded, executable segment. A malformed ELF redirects execution to an arbitrary address.

### H-12 ✅ FIXED — `src/libc/src/stdlib.c:83-93` — Heap coalescing uses `cur->next->size` unaligned

```c
cur->size = heap_align(cur->size) + sizeof(heap_block_t) + cur->next->size;
```

`cur->next->size` should be `heap_align(cur->next->size)`. The merged block is under-sized, causing subsequent allocations from the merged block to write past its declared end.

### H-13 ✅ FIXED — `src/services/fs_fat/fs_fat.c:516` — FAT32 `fat_size` reads only 16 bits of a 32-bit field

```c
uint16_t fat_size = ((uint16_t *)bpb->ext)[4];  // reads ext[8..9] only
```

The FAT32 `BPB_FATSz32` field at `ext[8]` is 32 bits. Only the low 16 bits are read, truncating the FAT size for large volumes. All subsequent LBA calculations (`g_root_dir_lba`, cluster numbering) are wrong.

### H-14 ✅ FIXED — `src/services/vt/vt_main.c:1247` — Canonical input drops UTF-8 bytes ≥ 0x80

```c
if (ch < 0x20 || ch > 0x7E) { return; }
```

All multi-byte UTF-8 continuation and leading bytes are silently discarded in canonical mode. German umlauts and any non-ASCII input are lost before reaching the application.

### H-15 ✅ FIXED — `src/libc/wasm/include/wasmos/libsys.h` — NULL dereference in `wasmos_sys_event_loop_poll`

```c
if (!loop || budget == 0) {
    budget = 1;
}
// falls through — dereferences loop->receiver_endpoint even when loop == NULL
```

### H-16 ✅ FIXED — `src/services/device_manager/device_manager.c:839` — PCI device index ≥ 64 bypasses the spawned-device bitmask

```c
if (di < 64u && ((rule->spawned_device_mask >> di) & 1u) != 0u) continue;
```

When `di >= 64`, the guard is always false. The device driver is spawned on every poll cycle.

### H-17 ✅ FIXED — `src/services/fs_manager/fs_manager.c:343` — `snprintf` size argument unsigned underflow

```c
snprintf(mounts + pos + n, sizeof(mounts) - (pos + (uint32_t)n), ...)
```

When `pos + n > sizeof(mounts)`, the `uint32_t` subtraction wraps to a huge value. `snprintf` treats this as enormous capacity and writes past the end of `mounts[384]`.

### H-18 ❌ FALSE POSITIVE — `src/services/pci_bus/pci_bus.c:91` — Incomplete error check for `svcLookupRetry`

Only -1 is checked. Return values -2 or -3 cause `devmgr_endpoint` to be cast to `uint32_t`, making it a garbage endpoint ID. All subsequent PCI IPC calls go to the wrong endpoint.

### H-19 ⚠️ REVERTED — `src/drivers/keyboard/keyboard.ts:46-49` — AUX byte not consumed before returning -1 (infinite spin)

When `KEYBOARD_AUX_FLAG` is set, `readScancode()` returns -1 without reading `KEYBOARD_DATA_PORT`. The byte stays in the controller output buffer, so every subsequent call sees the same AUX byte — infinite busy loop reading nothing.

### H-20 ✅ FIXED — `src/services/gfx_compositor/gfx_compositor.zig:1664` — Glyph cache codepoint truncated to `u8`

```zig
entry.codepoint = @intCast(codepoint & 0xFF);
```

Any codepoint ≥ 256 (em-dash, smart quotes, etc.) has its high bits discarded. Distinct codepoints sharing the same low byte collide in the cache, returning the wrong glyph.

---

## Medium Severity Bugs

### M-1 ✅ FIXED — `src/libc/src/math.c:53-58` — `cosf` has no argument reduction

The 6th-order Taylor expansion around 0 has no range reduction (mod 2π). For `|x| > ~0.5`, results visibly diverge; for `|x| > π`, results leave `[-1, 1]` entirely (e.g., `cosf(M_PI)` ≈ -1.32).

### M-2 ✅ FIXED — `src/libc/src/math.c:77-92` — `powf` returns `1.0f` for all fractional exponents

```c
if ((float)yi != y) { return 1.0f; }
```

`powf(2.0f, 0.5f)` returns `1.0f` instead of `~1.414f`. Complete logic failure for non-integer exponents.

### M-3 ✅ FIXED — `src/libc/src/math.c:62-74` — `acosf` wrong approximation (≈22% error at `x=0.9`)

The approximation `π/2 - x - x³/6` is only a 3-term asin approximation reflected; it diverges badly away from zero.

### M-4 ✅ FIXED — `src/libc/src/math.c:11-15, 33-36` — `floorf`/`ceilf`/`fmodf` UB for `|x| > INT_MAX`

Casting a `float` larger than `INT_MAX` to `int` is undefined behavior (WASM trap).

### M-5 ✅ FIXED — `src/libc/src/unistd.c:176` — `open()` treats `O_RDWR` as `O_RDONLY`

```c
access_mode = flags & O_WRONLY;   // masks only bit 0
```

`O_RDWR` (value 2) `& 1 == 0`, which matches `O_RDONLY`. Files opened with `O_RDWR` are silently opened read-only.

### M-6 ❌ FALSE POSITIVE — `src/libc/src/unistd.c:148-159` — IPC stream drops NUL bytes

```c
if (c == '\0') { continue; }
```

Binary data with embedded NUL bytes is silently discarded.

### M-7 — `src/libc/src/stdlib.c:51-53` — Overflow before bounds check in `heap_request_block`

`sizeof(heap_block_t) + payload_size` overflows `size_t` before `heap_align` is called. The check `total > 0xFFFFFFFF` only tests the aligned result.

### M-8 — `src/libc/src/stdlib.c:267` — `strtol` has no overflow detection

Overflow wraps silently without setting `errno`, giving wrong results for strings like `"99999999999999999999"`.

### M-9 ❌ FALSE POSITIVE — `src/libc/src/stdio.c:31` — `vsnprintf` off-by-one drops last character

`buffer->pos + 1 < buffer->size` prevents writing to `buffer[size-2]`. The NUL is written at `size-1` separately, meaning the penultimate position is never written — one character lost on a full buffer.

### M-10 — `src/libc/src/stdio.c:200-201` — `%lld` and `%zu` not handled

`%lld` is silently treated as `%ld` and `%zu` as `%d`, producing incorrect output when `long long` or `size_t` values are passed.

### M-11 — `src/boot/boot.c:607-648` — EFI pool and file handle leaks on all error paths in `read_file_alloc`

Every early-return error path in `read_file_alloc` leaks the `info` pool allocation or the `buf` allocation, and never calls `file->Close`. EFI boot-time resource exhaustion on repeated load failures.

### M-12 — `src/boot/boot.c:769,807-811` — ELF loader does not bounds-check `e_phoff` or `p_offset + p_filesz`

A malformed kernel ELF with `e_phoff` or `p_offset + p_filesz` beyond `kernel_size` causes an out-of-bounds read from the pool buffer.

### M-13 — `src/boot/boot.c:800-804` — ELF `PT_LOAD` array fixed at 16 entries with silent overflow

If the kernel has more than 16 `PT_LOAD` segments, the overflow is ignored. The overlap-check for subsequent segments fails to detect already-allocated regions, potentially double-allocating the same physical pages and corrupting the kernel image.

### M-14 ✅ FIXED — `src/services/font_service/font_service.zig:520-530` — Scratch shmem handle leaked every time a larger glyph is requested

The old `g_raster_scratch_shmem_id` is overwritten without calling `shmem_destroy`. Every upsizing call permanently leaks a shmem handle.

### M-15 — `src/drivers/framebuffer/framebuffer_native.c:113` — Ring buffer `read_pos` grows unboundedly

`ring->read_pos = rp` stores the raw non-modded counter. Eventually `read_pos` wraps past `UINT32_MAX` and the ring appears permanently non-empty, reading garbage memory beyond the `data` array.

### M-16 ❌ FALSE POSITIVE — `src/services/cli/cli.c:1397-1410, 1833-1841` — NUL separator between path and args never written to FS buffer

`write_off` is advanced past `path_len` to leave room for NUL, but the byte at `path_len` is never explicitly written. The kernel's exec reader splits path from args on NUL and reads uninitialized buffer content.

### M-17 ✅ FIXED — `src/kernel/process_manager_buffers.c` — Physical address returned as virtual pointer from `pm_fs_buffer_for_context`

`pfa_alloc_pages` returns a physical address. It is cast to `void *` and used as a virtual pointer. Once the identity (low) mapping is removed, all FS buffer accesses through this pointer fault or corrupt memory.

### M-18 ✅ FIXED — `src/services/gfx_compositor/gfx_compositor.zig:2159-2165` — Damage rect coordinates missing chrome/title-bar height offset

```zig
screen_rect.y = win.y + damage.y;   // should be win.y + CHROME_HEIGHT + damage.y
```

Every damage rect is placed `CHROME_HEIGHT` pixels too high on screen. The actual dirty content area is not composited.

### M-19 ✅ FIXED — `src/services/device_manager/device_manager.c:774-791,1040-1052` — `request_id` not incremented on `dm_ipc_call` failure

When `dm_ipc_call` returns -1, `g_dm.request_id` is not incremented. The next call reuses the same ID. A stale late reply matches the new request and the caller accepts wrong data.

### M-20 — `src/services/fs_init/fs_init.c:128-134` — Out-of-bounds read in `initfs_normalize_input_path`

`in[ri+1]` through `in[ri+4]` are accessed without checking that `in` has at least `ri+5` valid bytes. A short input string causes reads past the null terminator.

### M-21 — `src/services/vt/vt_main.c:1478` — VT_IPC_WRITE_REQ stops at first NUL byte

```c
if (b == 0) { break; }
```

NUL is a valid control character. Any write payload containing an embedded NUL has subsequent bytes silently dropped.

### M-22 — `src/services/vt/vt_main.c:844` — CSI accumulator `uint16_t` overflow

`csi_current * 10 + digit` wraps at 65535 with no diagnostic. A long numeric CSI parameter like `\e[65536m` is parsed as `\e[0m`.

### M-23 — `src/libsys/native/zig/libsys_native.c:655,679` — Signed integer overflow before cast in buffer helpers

`(uint32_t)(offset + len)` — both `int32_t`. Adding two large `int32_t` values overflows before the cast. Use `(uint32_t)offset + (uint32_t)len`.

### M-24 ❌ FALSE POSITIVE — `src/libsys/native/zig/libsys_native.c` — `wasmos_sys_ipc_recv_loop_native` loops forever on negative error code

Only `rc == 1` (IPC_EMPTY) is handled; `rc < 0` falls through to dispatch with a garbage message, then immediately retries — infinite CPU spin on IPC errors.

### M-25 ❌ FALSE POSITIVE — `src/kernel/process.c` — Stale `ctx->rsp` on user-mode preemption in `process_preempt_from_irq`

RSP is not updated from the IRQ frame for user-mode preemption. On next dispatch, `process_validate_context` sees the stale RSP below the higher-half boundary and kills the thread.

### M-26 ✅ FIXED — `src/kernel/list_linked.c:28-38` — Memory leak when `state` is NULL in `list_linked_alloc`

A node is allocated before checking `if (!state)`. On the NULL path the just-allocated node is never freed.

### M-27 ✅ FIXED — `src/kernel/memory.c` — `mm_shared_create` ID wrap-around can assign duplicate ID

`g_shared_next_id` is incremented without checking whether the new ID is already in use among the 16 live slots. A duplicate ID makes `mm_shared_get` return whichever it finds first; the other is permanently unreachable.

### M-28 ✅ FIXED — `src/services/device_manager/device_manager_rules.c:379-380` — `dm_rules_load_block_fs` unconditionally clears mount-ready flags

Calling `dm_rules_load_block_fs` on hotplug/re-poll resets `boot_mount_ready` and `user_mount_ready` even when they were already legitimately set, causing spurious re-mount attempts.

### M-29 ✅ FIXED — `src/kernel/native_driver.c:485,501,538` — `nd_shmem_create`, `nd_shmem_map`, and `nd_shmem_flush` use raw physical addresses as CPU pointers

Same pattern as M-17: `mm_shared_create` and `mm_shared_get_phys` return physical addresses, but `nd_shmem_create` stored that in `*out_ptr` and `nd_shmem_map` returned it directly as a `void *`. Font-service faulted at `cr2=0x0000008400000000` (the physical page base) because the virtual address was never mapped. `nd_shmem_flush` also passed the raw physical base to `memcpy`. Fixed by applying `| KERNEL_HIGHER_HALF_BASE` to all three.

### M-30 ✅ FIXED — `src/kernel/native_driver.c` + `src/kernel/process_manager_buffers.c` — M-17 fix broke DMA path and `nd_buffer_borrow` page-table mapping

After M-17, `pm_fs_buffer_for_context` returns `phys | KERNEL_HIGHER_HALF_BASE` (a kernel virtual address). Two callers needed the raw physical address: `process_manager_buffer_dma_map` (was handing a virtual address to the DMA engine, causing `[ata] dma read fallback rc=-3`) and `nd_buffer_borrow` (was passing a virtual address as the physical page frame to `paging_map_4k_in_root`). Fixed by adding `process_manager_buffer_phys_for_context` that always returns the physical address, and routing both callers through it.

---

## Low Severity / Minor Bugs

### L-1 — `src/services/fs_fat/fs_fat.c:519-520` — `data_sectors` unsigned underflow on corrupt BPB

When `reserved_sectors + fat_count*fat_size + root_dir_sectors > total_sectors`, the subtraction wraps to a huge value, causing wrong FAT type detection. The BPB corruption is silently hidden.

### L-2 — `src/services/fs_fat/fs_fat.c:2378-2390` — LFN 0xFFFF padding mishandling

0xFFFF padding bytes mid-sequence prematurely terminate the LFN string, silently discarding characters after a corrupt LFN entry.

### L-3 — `src/services/fs_fat/fs_fat.c:547` — Hardcoded "bytes/sector=512" log message regardless of actual sector size

Diagnostic log always prints 512 even for 1K/2K/4K sector volumes.

### L-4 — `src/services/fs_manager/fs_manager.c:272` — `backend_refresh_boot_meta` extracts wrong PCI byte for `device_fn`

`(uint8_t)((a1 >> 8) & 0xFF)` straddles the device and reserved fields. All mount log output shows device number 0.

### L-5 — `src/drivers/ata/ata.c:200-213` — Partial multi-sector write with no rollback

If `wasmos_block_buffer_copy` fails mid-write, prior sectors are already committed to disk with no recovery path.

### L-6 — `src/drivers/ata/ata.c:216-220` — `CACHE_FLUSH` error not detected

`ata_wait_not_busy` does not check `ATA_SR_ERR`. If the flush fails, the function returns 0 (success).

### L-7 — `src/drivers/mouse/mouse.ts:204` — `dy` negation overflow when `p2 == 0x80`

`-(-128)` overflows `i8` range back to -128 in i32 arithmetic. Mouse delta is wrong for this edge case.

### L-8 — `src/drivers/serial/serial.ts:95-96` — Busy spin on `ipc_recv` error

When `ipc_recv` returns negative, the outer `for(;;)` immediately retries — 100% CPU spin.

### L-9 — `src/services/cli/cli.c:1603-1609` — `g_pending_cd_path[32]` silently truncates paths > 31 chars

Absolute paths longer than 31 characters are truncated without error.

### L-10 — `src/boot/boot.c:640` — `read_file_alloc`: `info->FileSize` may be uninitialized if firmware sets `info_size` to 0 after GetInfo

Minor robustness issue on buggy firmware.

### L-11 — `src/libc/src/unistd.c:463, 489` — `fread`/`fwrite` multiplication overflow not checked

`size * nmemb` can wrap for large values, causing under-reads/writes silently.

### L-12 — `src/libc/src/unistd.c:358-359` — `stat`: `st_size` not validated as non-negative

A negative IPC error code stored into `st->st_size` yields a huge unsigned file size.

### L-13 — `src/libc/include/wasmos/libui.h:261` — Old shmem IDs leaked on buffer growth (acknowledged TODO)

The old shmem is abandoned on every capacity increase, depleting shmem IDs over time.

### L-14 — `src/libc/include/wasmos/libui.h:591` — `bytes` computation can overflow `int32_t` for very large windows

`new_stride * new_h` overflows for pathological window sizes. Failure is caught downstream but no bounds check.

### L-15 — `src/services/font_service/font_service.zig:850` — Negative `module_count` bitcast to invalid endpoint ID

`@bitCast(module_count)` where `module_count` is negative yields a large `u32`. If it's not `IPC_ENDPOINT_NONE`, subsequent IPC calls use a garbage endpoint.

### L-16 — `src/services/fs_init/fs_init.c:562-563` — initfs reads capped at 512 bytes despite 256 KB FS buffer

Callers requesting large reads must loop excessively and receive inconsistent chunk sizes.

### L-17 — `src/drivers/framebuffer/framebuffer_pci_native.c:308-310` — `g_fb_bytes_limit` set from pre-page-aligned size

A mode change requiring exactly one more page than the raw byte size (but within the page-rounded allocation) is wrongly rejected.

### L-18 — `src/kernel/libc.c` — `append_i64` UB on `INT64_MIN`

`(uint64_t)(-(value))` when `value == INT64_MIN` is signed overflow; works on most compilers but is not standard-defined.

### L-19 — `src/kernel/libc.c` — `vsnprintf` width accumulator integer overflow

No overflow check on `int width`. A format string like `%2147483647d` overflows `width` negative, producing incorrect output.

### L-20 — `src/services/vt/vt_main.c:86-113` — `vt_alloc(0)` returns current heap cursor without advancing it

Two consecutive zero-size allocations return the same pointer — aliased allocations.

### L-21 — `src/kernel/process_manager_spawn.c:` — Valid FS reply discarded in `pm_recv_fs_reply` on request_id mismatch

Out-of-order replies are consumed and lost; the FS operation hangs.

### L-22 — `src/kernel/process.c` — `in_ready_queue` not cleared on dequeue edge case

In specific preemption timing, a thread can be silently dropped from the run queue.

---

## Summary Table

| Subsystem | Critical | High | Medium | Low | Total |
|---|---|---|---|---|---|
| Kernel (ipc, memory, paging, process, arch) | 5 | 12 | 4 | 4 | 25 |
| Drivers (keyboard, mouse, serial, ata, framebuffer) | 0 | 1 | 0 | 4 | 5 |
| Services (vt, fat, fs_init, cli, device_mgr, gfx_comp) | 0 | 6 | 9 | 5 | 20 |
| Libc (stdio, stdlib, string, math, unistd, ipc.h) | 2 | 3 | 8 | 4 | 17 |
| Boot (boot.c) | 1 | 0 | 3 | 2 | 6 |
| libsys / libui | 0 | 2 | 3 | 3 | 8 |
| **Total** | **8** | **24** | **27** | **22** | **81** |

---

## SMP Concurrency Analysis (2026-06-03)

> Baseline: last known-good commit `50ef19f10a98287c595fe526d4462da298b32b80`  
> Covers all commits and uncommitted changes since that baseline.  
> Previous fix-commits patched symptoms; root causes are documented below.

### Lock Hierarchy (required, not yet enforced)

```
g_pfa_lock  (outermost)
  → g_endpoint_table_lock
    → ep->lock
      → g_process_table_lock
        → g_thread_table_lock
          → g_ready_queue_lock  (innermost)
```

Never acquire an outer lock while holding an inner one.

---

### SMP-CRIT-01 — `g_in_context_switch` is a single global

**File:** `src/kernel/process.c:40`

When CPU 0 sets `g_in_context_switch` during its own context switch,
`process_preempt_from_irq` on CPU 1 checks the same flag and incorrectly
suppresses preemption on CPU 1.

**Fix:** Move into `cpu_local_t`. Update `context_switch.S` to use a
GS-relative write. Test `cpu_local()->in_context_switch` in
`process_preempt_from_irq`.

---

### SMP-CRIT-02 — Thread table read without lock

**File:** `src/kernel/thread.c:166–229`

`thread_get`, `thread_find_main_for_pid`, `thread_owner_tid_at`,
`thread_mark_owner_exited` read `g_threads[]` without `g_thread_table_lock`.
Concurrent `thread_reap_owner` (under the lock) can zero a slot mid-iteration.

**Fix:** Split into `_locked` internal variant and a public wrapper.  Callers
already holding the lock call the internal variant.  `thread_set_state` (which
takes the lock and calls `thread_get`) must call the internal variant to avoid
re-entrant lock.

---

### SMP-CRIT-03 — Process table read without lock in scheduler hot path

**File:** `src/kernel/process.c:744–1855`

`process_find_by_pid`, `process_find_by_context_internal`,
`process_schedule_once_impl`, `process_kill`, `process_wait`,
`process_wake_by_context` all read `g_processes[]` without
`g_process_table_lock`.  Concurrent `process_reap` can reset a slot mid-read
→ corrupted context restore or page fault.

**Fix:** Hold `g_process_table_lock` for all table reads.  The scheduler must
not hold it across a context switch — copy needed fields to locals, release
lock, then switch.

---

### SMP-CRIT-04 — Use-after-free window during `process_reap`

**File:** `src/kernel/process.c:875–918`

`process_reap` frees stacks, IPC endpoints, and mm_context (steps 1–5)
*before* removing the slot from `g_processes[]`.  Any CPU calling
`process_find_by_pid` during this window gets a live pointer to freed memory.

**Fix:** Set `proc->state = PROCESS_STATE_REAPING` under `g_process_table_lock`
before beginning teardown so `process_find_by_pid` skips it.

---

### SMP-CRIT-05 — `process_wake_by_context` can schedule a RUNNING thread on two CPUs

**File:** `src/kernel/process.c:1567–1591`

The state check and `process_set_ready`/`ready_queue_enqueue` sequence are
not atomic.  A concurrent wakeup can put a RUNNING thread into the ready
queue, causing two CPUs to execute it simultaneously — instant stack/register
corruption.  The guard `proc->state == RUNNING && proc == cpu_local()->current_process`
is also wrong: the process could be RUNNING on a *different* CPU.

**Fix:** Hold `g_process_table_lock` across the entire sequence.  Verify the
thread is not RUNNING on any CPU before enqueuing.

---

### SMP-HIGH-01 — Slab allocator has no lock

**File:** `src/kernel/slab.c:66–103`

`kalloc_small`/`kfree_small` manipulate a global free-list with no lock and no
atomic operations.  Two CPUs can both dequeue the same slab chunk — classic
double-alloc.

**Fix:** Add `spinlock_t` to each `slab_class_t`; take it in
`kalloc_small`/`kfree_small`.

---

### SMP-HIGH-02 — Shared `g_mm_copy_stack` used as stack by all CPUs

**File:** `src/kernel/memory.c:26,73–99`

`mm_run_on_copy_stack` switches RSP to the top of a single 8 KB static buffer.
Two CPUs executing this path simultaneously corrupt each other's frames.

**Fix:** Move into `cpu_local_t`, or use the existing per-CPU
`g_sched_trampoline_stacks`.

---

### SMP-HIGH-03 — `g_wasm_driver_registry` guarded by `preempt_disable` only

**File:** `src/kernel/wasm_driver.c:96–186`

`preempt_disable()` increments a per-CPU counter only.  Two CPUs can
simultaneously enter `wasm_driver_registry_set` and `wasm_driver_registry_get`.

**Fix:** Replace `critical_section_enter/leave` with
`spinlock_lock/unlock(&g_wasm_driver_registry_lock)`.

---

### SMP-HIGH-04 — `process_wake_thread` double-enqueue race

**File:** `src/kernel/process.c:1600–1619`

`thread->state` is read locklessly.  Two CPUs can both see `THREAD_STATE_BLOCKED`,
both call `process_set_ready`, both try to enqueue the same thread.

**Fix:** Hold `g_thread_table_lock` across the full read-check-set-enqueue
sequence.

---

### SMP-HIGH-05 — PID and mm_context leaked on spawn error paths

**File:** `src/kernel/process.c:993,1120`

`g_next_pid` is incremented and `mm_context_create` is called before the slot
and name copy are validated.  Error returns after that point orphan the PID
and context.

**Fix:** Validate slot and name before incrementing PID and creating context.

---

### SMP-HIGH-06 — Thread counts incremented without `g_process_table_lock`

**File:** `src/kernel/process.c:1203–1244`

`process_thread_spawn_worker_internal` reads `owner->state`/`owner->exiting`
and increments `thread_count`/`live_thread_count` without holding the lock.
A concurrent kill can zombie the owner between the check and the increment.

**Fix:** Take `g_process_table_lock` for the entire check-and-increment.

---

### SMP-MED-01 — No enforced lock hierarchy → latent deadlocks

As SMP-CRIT fixes land, new lock sites will be added.  Without a documented
hierarchy, a deadlock cycle will close.  The hierarchy above (Lock Hierarchy
section) must be audited against every call path.

---

### SMP-MED-02 — `ep->lock` held across `process_block_on_ipc`

**File:** `src/kernel/ipc.c:317–391`

`ep->lock` is held while calling `process_block_on_ipc` → `thread_set_state`
→ `g_thread_table_lock`.  Any future wakeup path that sends IPC under the
thread lock will close the cycle: `ep→thread→ep`.

---

### SMP-MED-03 — Recursive spinlock check is non-atomic

**File:** `src/kernel/spinlock.c:85–106`

`lock->state` and `lock->owner_cpu` are read as two separate non-atomic loads.
The recursive spinlock is also a workaround for an unfixed reentry root cause
that must be eliminated.

**Fix:** `__atomic_load_n(..., __ATOMIC_ACQUIRE)` for `owner_cpu` after
observing `state`.  Then fix the reentry root cause and remove the recursion.

---

### SMP-MED-04 — IRQ-path `ipc_send_from` uses recursive spinlock as workaround

**File:** `src/kernel/ipc.c:225–228`

If the same CPU already holds `ep->lock` (in `ipc_recv_blocking_for`), the
recursive check fires and `recursion_depth` is incremented.  The IRQ handler's
"release" does not fully release the lock, leaving it permanently held.

**Fix:** Separate the ISR-safe send path (lockless ring or per-endpoint ISR
slot) from the blocking receive path.  Remove recursive spinlock after.

---

### SMP-MED-05 — `process_preempt_from_irq` races with `process_reap`

**File:** `src/kernel/process.c:1908–2034`

State check at line 1937 and `process_set_ready` at line 2028 are not atomic.
Concurrent `process_reap` can zombie the process between them → zombie thread
scheduled and executed.

**Fix:** Atomically check `state == RUNNING` and transition under
`g_process_table_lock`.

---

### SMP-MED-06 — `blocking_transition` cleared without memory barrier

**File:** `src/kernel/ipc.c:372`, `src/kernel/process.c:1815`

`thread->blocking_transition = 0` is a plain store.  A concurrent CPU reading
it in `ready_queue_dequeue` may see a stale `1` after the flag is cleared.

**Fix:** `__atomic_store_n(..., 0, __ATOMIC_RELEASE)` / `__atomic_load_n(...,
__ATOMIC_ACQUIRE)`.

---

### SMP-MED-07 — Notification blocking path has spurious-miss race

**File:** `src/kernel/ipc.c:423–447`

`ipc_wait_for` arms the waiter and returns `IPC_EMPTY`.  A sender can fire
between the `ep->lock` release and the caller's external `process_block_on_ipc`
call.  The notification is delivered to a RUNNING thread, ignored, and the
thread blocks forever.

**Fix:** `ipc_wait_blocking_for` that arms + blocks + checks atomically under
`ep->lock`, mirroring `ipc_recv_blocking_for`.

---

### SMP-MED-08 — `wake_waiters` and `process_info*` iterate table without lock

**File:** `src/kernel/process.c:771–861, 2088–2257`

`process_wake_waiters`, `process_has_waiters`, `process_count_active`,
`process_info_at*`, `process_ready_count` all iterate `g_processes[]` without
`g_process_table_lock`.

---

### SMP-LOW-01 — Shared diagnostic counters with lost updates

**File:** `src/kernel/process.c:54–55,1733–1735`

`g_sched_switch_count`, `g_sched_progress_logged`, etc. written from all CPUs
without synchronization.

**Fix:** `__sync_fetch_and_add` / `__sync_bool_compare_and_swap`, or per-CPU.

---

### SMP-LOW-02 — `g_idle_process` needs store-release

**File:** `src/kernel/process.c:1109`

Written under `g_process_table_lock` but read without lock on every CPU.

**Fix:** `__sync_synchronize()` after the write in `process_spawn_idle`.

---

### SMP-LOW-03 — AP `started` flag polled without acquire barrier

**File:** `src/kernel/arch/x86_64/smp.c:190`

Correct on x86 TSO but not self-documenting.

**Fix:** `__atomic_load_n(&ap->started, __ATOMIC_ACQUIRE)` on BSP poll side.

---

### SMP-LOW-04 — `cli` loop in scheduler is misleading under SMP

**File:** `src/kernel/kernel_boot_runtime.c:158–170`

`cli` only suppresses IRQs locally; it does not protect shared state from other
CPUs' schedulers, but the structure implies it does.

---

## Uncommitted Changes Verdict (2026-06-03)

### KEEP — Correct Fixes

| File | Change |
|------|--------|
| `src/kernel/include/arch/x86_64/smp.h` | `wasm3_heap_bound_pid` moved into `cpu_local_t` — fixes SMP-HIGH-03 |
| `src/kernel/include/thread.h` | `wasm3_heap_bound_pid` added to `thread_t` for save/restore across context switches |
| `src/kernel/thread.c` | Reset `wasm3_heap_bound_pid` in `thread_reset_slot` |
| `src/kernel/wasm3_shim.c` | `critical_section_enter/leave` → `spinlock_lock/unlock(&g_wasm3_heap_lock)` + per-CPU bound-pid — correct SMP fix |
| `src/kernel/memory.c` | Read CR3 from register (`mov %%cr3, %0`) instead of `g_current_pml4_phys` — latter is last-writer-wins under SMP |
| `src/kernel/kernel_boot_runtime.c` | Remap initfs to higher-half virtual alias — physical pointer becomes invalid after identity map strip |
| `src/kernel/process.c` | Save `wasm3_heap_bound_pid` to thread on yield, restore from thread on schedule |
| `src/kernel/process.c` | Resume blocked kernel workers via `context_switch_high` when `proc->ctx.rsp != 0`; clear `ctx.rsp` on exit |
| `src/kernel/wasm_driver.c` | Exit via `process_yield(PROCESS_RUN_THREAD_EXITED)` — paired with above; returning is unsafe from a resumed context |
| `src/services/device_manager/device_manager.c` | `queue_acpi_match_rule_spawns` early-return guard (`if rule_spawn_pending return`) — prevents double-queuing |
| `src/services/device_manager/device_manager.c` | `next_spawn_target` guard: `if (boot_mount_ready && !rules_boot_loaded) return HW_SPAWN_NONE` — prevents `poll_boot_rules_async` from resetting rule tables mid-spawn |
| `src/services/device_manager/device_manager.c` | Removal of previously committed `[dbg-dm]` blocks from `queue_pci_match_rule_spawns` and `apply_pci_matches` — cleanup of old debug noise |

### REVERT / REMOVE — Wrong Fixes

| File | Change | Why |
|------|--------|-----|
| `src/drivers/fs_fat/fs_fat.c` | ~~Retry loop (500 attempts) for devmgr block-mount query~~ | **Logically impossible scenario.** The rule that spawns FAT #2 is physically stored in a file on FAT #1's `/boot` filesystem. DM reads that file to get the spawn rule. The act of spawning FAT #2 *is* the rule being executed — DM cannot spawn FAT #2 without already having the rule in memory. The retry was masking an IPC delivery failure (likely BUG-20 / recursive spinlock leaving `ep->lock` permanently held). |
| `src/services/device_manager/device_manager.c` | ~~`kick_boot_rules_read_async`/`poll_boot_rules_async` calls inside `dm_ipc_call` loop~~ | Same wrong premise. If DM is blocked in `dm_ipc_call` spawning FAT #2, boot rules are already loaded (they had to be, to have the FAT #2 spawn rule). Calling boot-rule loading inside the spawn loop was unnecessary. **Already removed from working tree.** |

### SUSPICIOUS — Possible Workaround

| File | Change | Concern |
|------|--------|---------|
| `src/services/device_manager/device_manager.c` | `DM_SPAWN_TIMEOUT_MS: 5000 → 30000` | A 6× timeout increase suggests spawns were timing out — likely because underlying IPC/SMP bugs slow message delivery. May be masking the same root cause. Should be reverted once IPC bugs are fixed and spawn latency returns to normal. |

### REMOVED (debug noise, done)

`[dbg-ata]` printfs in `ata.c`, `[dbg-fat]` logs in `fs_fat.c`, `[dbg-dm]` blocks in `device_manager.c` — all stripped.

### NEUTRAL — No Runtime Effect

| File | Change |
|------|--------|
| `src/drivers/fs_fat/fs_fat.c` | Split combined `send && recv` into two separate `if` checks — clearer error paths, no behavioral change |
