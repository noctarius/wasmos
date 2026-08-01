# Known Bugs

Two issues surfaced while building the Rust Tetris example
(`examples/rust/tetris`). Neither is a Tetris-specific defect — the game is the
first keyboard-heavy, large-window, networked app and simply exposes pre-existing
system gaps. Single-player Tetris works end-to-end.

---

## 1. Keyboard is consumed by both the CLI/VT and the compositor's focused window — FIXED

**Fix.** Adopted the CLI-side option below. `cli_is_foreground()`
(`src/services/cli/cli.c`) now always records the *actual* active tty returned
by the VT query instead of pinning `g_last_seen_active_tty` to `g_home_tty`
whenever the compositor owns `tty0`. So while the compositor is foreground
(`active_tty == 0`), the CLI reports itself background and stops reading the
emulated keyboard; the compositor's focused window gets keys exclusively. When
the compositor releases `tty0`, the frequent (backoff `3`) foreground re-query
restores the CLI. Serial-driven input is unaffected: once the CLI falls back to
serial (`g_vt_endpoint < 0`) it is always foreground via the early return in
`cli_is_foreground()`, so `run-qemu-cli-test` still drives commands over serial.

Original report retained below for context.

**Symptom.** While a compositor (graphical) window is the foreground on `vt0`,
keystrokes are *also* delivered to the CLI on its shell tty. Playing Tetris with
`WASD`/space leaves a trail of `aaassdd…` at the `wamos>` prompt. A related
artifact: the framebuffer text console occasionally flashes for a frame before
the compositor re-composites over it.

**Root cause.** There is no unified keyboard-focus arbitration between the
compositor and the virtual-terminal/CLI:

- The PS/2 keyboard driver (`src/drivers/keyboard/keyboard.ts`) *broadcasts*
  `KBD_IPC_KEY_NOTIFY` to every subscriber. Both the compositor
  (`src/services/gfx_compositor/gfx_compositor.zig`, `subscribe_keyboard`) and
  the VT service (`src/services/vt/vt_main.c:1294`) subscribe, so both receive
  every key.
- The compositor delivers keys only to the *focused* window and switches the VT
  to `tty0` while it owns the display (`try_switch_to_gfx_tty`); `tty0` is
  read-only for shell input. But the CLI (`src/services/cli/cli.c`) keeps
  treating itself as foreground: when it queries the active tty and sees the
  compositor owns `tty0`, the guard at `cli.c:~711`
  (`if (!(g_home_tty == 1 && active_tty == 0)) …`) leaves
  `g_last_seen_active_tty` at its default (`1 == g_home_tty`), so
  `cli_is_foreground()` stays true and the CLI keeps reading keyboard input
  from its tty.

**Why Tetris exposes it.** Earlier graphical examples (`gfx_smoke`, `menu_bar`,
`calculator`) barely use the keyboard, so nobody noticed the double delivery.

**Suggested fix (safe for tests).** When the compositor owns the foreground
(overlay locked / a window is focused on `tty0`), the CLI should consider itself
*background* and stop consuming keyboard input — it already falls back to
**serial** for input (`[cli] vt read failed; serial fallback`), and the
automated CLI tests drive commands over serial (`QemuSession.send` writes to the
QEMU serial `stdin`), so suppressing the emulated-keyboard path does **not**
break `run-qemu-cli-test`. Options:
- CLI-side: treat `active_tty == 0` (compositor foreground) as "not foreground"
  for input purposes (one-line change to the `cli.c:~711` guard), or
- Compositor→VT signal: have the compositor tell the VT to suspend
  shell-keyboard delivery on overlay lock and resume on unlock.

**Files.** `src/services/cli/cli.c`, `src/services/vt/vt_main.c`,
`src/services/gfx_compositor/gfx_compositor.zig`,
`src/drivers/keyboard/keyboard.ts`.

---

## 2. Networked 2-player: host accept never pairs with the incoming connection — FIXED

**Fix.** Two independent defects were behind this; both are fixed and a 2-player
match now starts (verified end-to-end: host on one guest, a peer connecting through
the SLIRP `hostfwd` → `[tetris] match start` on the host).

1. **Blocking handshake.** The host path drove listen/accept with blocking IPC and
   stalled the whole process in `wasmos_net__recv_reply` waiting for the deferred
   accept. It is now a **non-blocking, doorbell-driven** handshake. New shared
   primitives live in `src/libc/include/wasmos/net.h`
   (`wasmos_net_tcp_listen_begin` / `wasmos_net_tcp_connect_begin` +
   `wasmos_net_tcp_advance`, a doorbell handler — not a poll). `net_shim.c` is a
   thin adapter; `tetris.rs` adds a `Phase::Connecting` that steps the handshake
   from the existing event loop whenever the `reply_ep` doorbell wakes it, so the
   window keeps rendering "WAITING FOR OPPONENT" and nothing blocks.

2. **`#UD` on ring setup.** Once non-blocking, the host crashed with an invalid-
   opcode fault while overlaying the accepted socket's data rings. Root cause: clang
   compiles `wasmos_ringbuf_is_pow2()`'s `(x & (x-1)) == 0` power-of-two idiom to
   `i32.popcnt`; WARP's JIT lowers that to the x86 `POPCNT` instruction; and the
   QEMU commands passed **no `-cpu`**, so the default `qemu64` model does not
   advertise POPCNT → `#UD`. (Other WARP apps fold their pow2 checks to constants;
   tetris passes a runtime ring capacity.) Fixed by advertising a modern CPU
   (`-cpu max`) in the run/test QEMU commands (CMake `WASMOS_QEMU_CPU_ARGS` +
   `scripts/qemu_test_framework.py`), matching the x86-64 baseline WARP's codegen
   assumes.

A third red herring surfaced during debugging: `QemuSession.start()` dropped
`nic_model`/`netdev` when rebuilding the config for monitor mode, so a
monitor-driven test lost its `hostfwd` and saw a spurious "connection refused".
That framework bug is also fixed.

Original report retained below for context.

**Symptom.** Choosing **BE HOST** (or pressing `h`) opens a TCP listener on
`:7000` and shows `WAITING FOR OPPONENT`. When a peer connects (through a QEMU
SLIRP `hostfwd`), net-stack never pairs the connection with the posted accept
slot: the deferred `NET_IPC_ACCEPT` reply never arrives and `tnet_host` blocks
forever, so the match never starts. **JOIN** (client) uses the shared `net.h`
connect helper.

**What was verified.** With serial-driven instrumentation, the host path reaches
every step successfully — service lookup, `NET_IPC_SOCKET_OPEN` (listen socket
id `0`, which is valid — net-stack ids are 0-based), `NET_IPC_BIND :7000`,
`NET_IPC_LISTEN`, and the `NET_IPC_ACCEPT` post — and then blocks in the deferred
accept receive. No error reply comes back; net-stack simply never fires the
accept pairing for the inbound SYN. eth0 comes up (`10.0.2.15/24 ready`) and the
NIC RX is IRQ-driven, so delivery is not the issue.

**Key comparison.** The *identical* raw `SOCKET_OPEN → BIND → LISTEN → ACCEPT`
sequence in `examples/c/net_tcp_server/net_tcp_server.c` passes its e2e test
(`tests/test_net_stack_tcp_server_e2e.py`), so the platform accept path works.
The one structural difference: the Tetris shim **maps** the accept rings into
linear memory (`xfer_buffer_map` + `wasmos_ringbuf_init`) *before* posting the
accept, whereas `net_tcp_server` keeps them unmapped and pokes them with
`xfer_buffer_read/write`. Suspected interaction between the app-mapped accept
rings and net-stack's `xfer_buffer_map_borrowed` on the same pages in
`net_stack_handle_accept` (or the gfx+net coexistence in one process).

**Architectural note.** The shim drives the whole host handshake with **blocking**
IPC (`wasmos_net__recv_reply` → `ipc_select_one`, which blocks in-hostcall under
WARP). The platform provides a coroutine/future async runtime (`src/libsys`,
with Rust bindings in `src/libc/rust/coroutine.rs`); the intended design is a
non-blocking event-loop that posts the accept and awaits its reply in the main
select set alongside the compositor event endpoint, rather than blocking the
whole process. Reworking the host path onto that async model is the right
long-term fix and would also let the window keep rendering "waiting for
opponent" instead of freezing.

**Status.** Single-player is fully functional. Host/Join code is present and
follows the tested primitives; the accept-pairing bug is open.

**Files.** `examples/rust/tetris/net_shim.c` (`tnet_host`),
`src/services/net_stack/net_stack.c` (`net_stack_handle_accept`,
`net_stack_tcp_accept`), `src/libc/include/wasmos/net.h`.

---

## Reproducing / testing the network path

Boot with a forwarded port and drive the game over serial + monitor (the
compositor only delivers keys to the *focused* window, so the app self-focuses
via `GFX_IPC_FOCUS_WINDOW` on launch):

```
qemu-system-x86_64 -m 512M -smp 4 -display none -vga std \
  -drive if=pflash,format=raw,readonly=on,file=<OVMF_CODE> \
  -drive format=raw,file=fat:rw:build/esp \
  -drive format=raw,file=fat:rw:userfs \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:7000-10.0.2.15:7000 \
  -device virtio-net-pci,netdev=net0,id=nic0 \
  -serial <bidirectional> -monitor <socket>
# in the guest CLI:  spawn /boot/apps/tetris
# then send key 'h' (host) and connect a peer to 127.0.0.1:7000
```
