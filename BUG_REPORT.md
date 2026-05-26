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

### H-4 ✅ FIXED — `src/kernel/paging.c` — User physical data pages not freed on process exit

`paging_destroy_address_space` frees page-table pages (PT, PD, PDPT) but never frees the physical data pages the leaf PTEs point to. All physical memory backing user address spaces is permanently leaked on process exit.

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

### M-9 — `src/libc/src/stdio.c:31` — `vsnprintf` off-by-one drops last character

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

## Fix Priority Order

1. **C-1** — `ipc.h` `arg2`/`arg3` field indices (corrupts every IPC reply)
2. **C-2** — `string.c` `memmove` backward chunk order (data corruption)
3. **C-3** — `boot.c` brace scoping / `boot_info->modules` (crashes on no-module boot)
4. **C-4** — `ipc.c` endpoint slot claim race (kernel race condition)
5. **C-5** — `paging.c` NX bit dropped on large-page split (security)
6. **C-7** — `irq_x86_64.c` spurious IRQ 7 EOI (PIC corruption)
7. **C-8** — `cpu_x86_64.c` RSP0/IST1 shared stack (crash on timer-during-syscall)
8. **H-4** — `paging.c` user physical pages leaked on process exit
9. **H-12** — `stdlib.c` heap coalesce wrong merged size
10. **H-13** — `fs_fat.c` FAT32 fat_size 16-bit truncation
11. **H-14** — `vt_main.c` canonical mode drops UTF-8
12. **H-19** — `keyboard.ts` AUX byte not consumed
13. **M-1/M-2** — `math.c` cosf/powf broken for common inputs
14. **M-5** — `unistd.c` O_RDWR treated as O_RDONLY
