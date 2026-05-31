# Virtual Input Testing via Virtio-Serial

## 1. Scope and Goals

This document defines a testing-focused virtual input path (mouse + keyboard)
for WASMOS that is controllable from the host via `virtio-serial`.

Goals:
- Provide deterministic UI input injection for automated tests and agent-driven debugging.
- Reuse the existing UI/input pipeline so injected events behave like hardware events.
- Keep guest attack surface small by avoiding guest-network-facing control channels.
- Integrate with existing Python QEMU test framework and test targets.

Non-goals:
- Replacing real mouse drivers for production/user workflows.
- Introducing a generic remote-control daemon exposed over guest TCP.
- Implementing full HID emulation in the first iteration.

## 2. Architecture Overview

Runtime components:
- Host test runner / bridge client (Python tests, local tools, agents).
- QEMU `virtio-serial` transport with a dedicated named port.
- Guest `virtio-serial` driver support.
- Guest `virt-input` service/driver endpoint.
- Existing compositor/input dispatch path.

Data flow:
1. Host sends text commands over a QEMU chardev bound to a `virtio-serial` port.
2. Guest `virtio-serial` driver reads framed bytes and routes them to `virt-input`.
3. `virt-input` parses and validates commands, then emits normalized pointer
   and keyboard events.
4. Compositor receives events through the existing input contract and dispatches them.

Control and security model:
- Injection is disabled by default and enabled only for test/debug configurations.
- Only the `virt-input` endpoint can convert host control messages into UI events.
- Parser is fail-closed; malformed commands are rejected with explicit error replies.

## 3. QEMU Host Bridge Setup

Recommended QEMU wiring:

```sh
-device virtio-serial-pci \
-chardev socket,id=virtinput,path=/tmp/wasmos-virtinput.sock,server=on,wait=off \
-device virtserialport,chardev=virtinput,name=wasmos.virtinput
```

Notes:
- Use one dedicated named port (`wasmos.virtinput`) for this feature.
- Unix domain sockets are preferred for local test orchestration.
- Keep the bridge local-only by default; optional TCP adapters live outside guest.

Test isolation:
- Use per-test socket paths (for example under temporary directories) to avoid collisions.
- Ensure stale socket files are removed before launch.
- Do not run UI/QEMU integration tests in parallel against shared `build/esp`.

## 4. Virtio-Serial Driver Requirements (Guest)

Required capabilities:
- Initialize `virtio-serial` transport and enumerate available ports.
- Match named port `wasmos.virtinput`.
- Provide a byte-stream read/write interface to consumers (`virt-input` service).
- Handle disconnect/reconnect events without kernel panic or deadlock.

Implementation constraints:
- Keep transport logic minimal and explicit (no extra framework layers).
- Bound all buffers and copy lengths.
- Use non-blocking or timeout-bounded waits suitable for the current scheduler/IPC model.
- Add targeted temporary marker logs (`[dbg-virtinput]`) only during bring-up and remove later.

## 5. Guest Virtual Input Service (`virt-input`)

Responsibilities:
- Open/register the `wasmos.virtinput` stream from the virtio-serial driver.
- Parse line-delimited command frames.
- Validate command syntax/ranges and normalize coordinates/key data.
- Emit pointer/button/wheel/keyboard events through the existing
  input/compositor interface.
- Return acknowledgements/errors to host for deterministic test sequencing.

Integration rule:
- Injected events must traverse the same compositor-focused event path as hardware events.
- No compositor bypass for test-only fast paths.

Reliability and safety:
- Per-session sequence tracking for `SYNC` acknowledgements.
- Fixed queue depth with explicit backpressure (`ERR BUSY`).
- Rate limiting to prevent test-host floods from stalling UI processing.

## 6. Text Protocol v1

Transport:
- UTF-8 text lines terminated by `\n`.
- One command per line.
- Unknown commands return an error response.

Command grammar:

```text
HELLO <version>
MOVE_ABS <x> <y>
MOVE_REL <dx> <dy>
BTN <left|right|middle> <down|up>
WHEEL <dy>
KEY <code> <down|up>
TEXT <utf8-text>
MOD <shift|ctrl|alt|meta> <down|up>
SYNC <token>
PING <token>
```

Responses:

```text
OK <optional-data>
ERR <code> <message>
PONG <token>
SYNCED <token>
```

Semantics:
- `HELLO` negotiates protocol major/minor (start with `1`).
- `MOVE_ABS` uses compositor-space absolute coordinates.
- `MOVE_REL` applies signed deltas.
- `BTN` changes one button state atomically.
- `WHEEL` uses signed vertical delta steps.
- `KEY` injects a key transition using a shared keycode namespace.
- `TEXT` injects UTF-8 text input for focused text-entry paths.
- `MOD` sets modifier state without requiring paired scan-code synthesis in v1.
- `SYNC` guarantees all prior accepted commands have been injected before `SYNCED`.

Validation:
- Numeric parsing must reject overflow and out-of-range values.
- Unknown buttons/keys/modifiers/states are hard errors.
- Empty lines and comments (optional `#`) may be ignored.

## 7. Capability and Gating

Build-time gating:
- Add Kconfig/CMake feature toggle (for example `WASMOS_VIRTINPUT_TESTING`).
- Default off in regular boots; enabled in test/debug targets.

Runtime gating:
- `virt-input` starts only when testing toggle is enabled.
- Optional runtime policy gate via startup configuration (for example `sysinit.rc`).

Threat model boundary:
- Host side is trusted for test runs.
- Guest still validates all input to preserve robustness under malformed streams.

## 8. Python Test Framework Integration

### 8.1 `qemu_test_framework.py` Extensions

Add optional fields to `QemuConfig`:
- `virtinput_socket_path: str = ""`
- `virtinput_port_name: str = "wasmos.virtinput"`
- `enable_virtinput: bool = False`

`build_qemu_cmd()` behavior when `enable_virtinput`:
- Add `-device virtio-serial-pci`.
- Add `-chardev socket,...` for configured socket path.
- Add `-device virtserialport,...,name=<virtinput_port_name>`.

Environment overrides (proposed):
- `WASMOS_QEMU_VIRTINPUT=1`
- `WASMOS_QEMU_VIRTINPUT_SOCK=/tmp/wasmos-virtinput.sock`
- `WASMOS_QEMU_VIRTINPUT_PORT=wasmos.virtinput`

### 8.2 Host Bridge Helper in Tests

Provide a small Python helper class (proposed):
- Connects to Unix socket with timeout/retry.
- Sends command lines and waits for `OK`/`ERR`.
- Exposes convenience methods: `move_abs`, `move_rel`, `click`, `wheel`,
  `key_down`, `key_up`, `text`, `sync`.

Deterministic sequencing:
- Use `SYNC <token>` between action phases.
- Keep assertions based on existing serial logs or UI-observable markers.

### 8.3 UI Test Usage

`scripts/qemu_ui_test.py` and/or dedicated UI integration tests can:
- Boot with `enable_virtinput`.
- Wait for system ready marker.
- Drive pointer actions through bridge helper.
- Assert expected UI state transitions in serial output markers.

## 9. Rollout Plan

Phase 1: transport and stubs
- Add `virtio-serial` driver skeleton and named-port discovery.
- Start `virt-input` service with protocol parser stub and `PING`/`HELLO`.

Phase 2: injection path
- Wire `MOVE_*`, `BTN`, `WHEEL`, `KEY`, `TEXT`, and `MOD` into existing
  input/compositor event path.
- Add `SYNC` barrier and queue/backpressure handling.

Phase 3: test harness integration
- Extend Python QEMU framework and add one end-to-end UI automation smoke test.
- Add targeted fault-path tests for malformed commands and overflow handling.

Phase 4: hardening
- Add rate limiting and clear diagnostics for dropped/rejected events.
- Finalize docs and remove temporary bring-up markers.

## 10. Validation and Regression Criteria

Must pass:
- Existing `run-qemu-test` baseline unchanged when feature is disabled.
- Existing UI smoke path unchanged when no host virt-input client is connected.
- New UI automation tests reliably perform pointer and keyboard interactions via
  `virtio-serial`.
- Malformed protocol lines do not crash driver/service/kernel and produce `ERR`.

Operational checks:
- No hangs on host disconnect/reconnect.
- No input queue growth without bounds.
- No bypass of compositor ownership/focus checks.
