## Python QEMU Test Framework

This document describes the Python test automation framework used for
integration testing in WASMOS.  All test scripts and the shared library live
in `scripts/`.  Integration test cases live in `tests/`.

---

### Overview

The framework drives a real QEMU virtual machine, observes its serial output,
and optionally controls its keyboard and mouse via the QEMU Machine Protocol
(QMP).  All test assertions are based on runtime behaviour — serial output
markers, exit codes, or state transitions — not on source text.

Three components make up the framework:

| Component     | Source                           | Role                                              |
|---------------|----------------------------------|---------------------------------------------------|
| `QemuConfig`  | `scripts/qemu_test_framework.py` | Describes a QEMU session (firmware, ESP, monitor) |
| `QemuSession` | `scripts/qemu_test_framework.py` | Launches QEMU, exposes serial I/O and expect API  |
| `QemuMonitor` | `scripts/qemu_test_framework.py` | QMP client for keyboard/mouse injection           |

---

### QemuConfig

`QemuConfig` is a Python dataclass that fully describes a QEMU session.

```python
@dataclass
class QemuConfig:
    ovmf_code:       str           # UEFI code firmware (required)
    ovmf_vars:       str           # UEFI variable store (optional)
    esp_dir:         str           # EFI System Partition directory
    userfs_dir:      str  = ""     # Optional secondary FAT drive
    nographic:       bool = True   # Headless mode (-nographic)
    display:         str  = ""     # Display backend (gtk, sdl, cocoa, …)
    isolate_esp:     bool = False  # Copy ESP to a temp dir before launch
    enable_monitor:  bool = False  # Auto-create a QMP socket
    monitor_socket:  str  = ""     # Explicit QMP socket path
```

#### Environment overrides

`default_config(build_dir="build")` reads the CMake cache and then applies
environment variable overrides:

| Variable                   | Field            | Notes                                     |
|----------------------------|------------------|-------------------------------------------|
| `WASMOS_OVMF_CODE`         | `ovmf_code`      | Overrides CMake cache `OVMF_CODE`         |
| `WASMOS_OVMF_VARS`         | `ovmf_vars`      | Overrides CMake cache `OVMF_VARS`         |
| `WASMOS_ESP`               | `esp_dir`        | Default: `<build_dir>/esp`                |
| `WASMOS_USERFS`            | `userfs_dir`     | Default: `<source_dir>/userfs`            |
| `WASMOS_QEMU_ISOLATE_ESP`  | `isolate_esp`    | Set to `1` to enable per-test ESP copies  |
| `WASMOS_QEMU_MONITOR`      | `enable_monitor` | Set to `1` to enable the QMP socket       |
| `WASMOS_QEMU_MONITOR_SOCK` | `monitor_socket` | Explicit socket path (implies monitor on) |

#### ESP isolation

When `isolate_esp=True`, `QemuSession.start()` copies the ESP directory into a
fresh temporary directory before launch.  This prevents one test's filesystem
writes from affecting the next.  The copy is cleaned up via `atexit`.  The
`run-qemu-cli-test` CMake target sets `WASMOS_QEMU_ISOLATE_ESP=1` so every
unittest in `tests/` gets an isolated view.

---

### QemuSession

`QemuSession` wraps a `subprocess.Popen` for `qemu-system-x86_64` and
accumulates its stdout into a grow-only byte buffer (`self.buf`).  QEMU is
launched with `-serial mon:stdio` so serial output and the embedded HMP
monitor share the same pipe.

#### Lifecycle

```python
# Context manager (recommended)
with QemuSession(cfg, timeout_s=120) as session:
    ...

# Manual
session = QemuSession(cfg)
session.start()   # launches QEMU, optionally connects QemuMonitor
# … test body …
session.close()   # sends Ctrl-A x, terminates process, cleans up
```

`session.monitor` is `None` unless `cfg.enable_monitor` or
`cfg.monitor_socket` is set, in which case it is a connected `QemuMonitor`
instance.

#### Serial output API

| Method                                  | Returns | Description                                                |
|-----------------------------------------|---------|------------------------------------------------------------|
| `send(line)`                            | —       | Write a line to QEMU's stdin (serial input)                |
| `expect(needle, timeout_s)`             | `bool`  | Wait until `needle` appears anywhere in `buf`              |
| `expect_from(start, needle, timeout_s)` | `bool`  | Like `expect`, but only searches `buf[start:]`             |
| `mark()`                                | `int`   | Return `len(buf)` — a cursor for later `expect_from` calls |
| `tail(max_bytes)`                       | `str`   | Last N bytes of output for failure diagnostics             |
| `force_stop()`                          | —       | Send `Ctrl-A x` to kill QEMU immediately                   |

`needle` may be `bytes`, `str`, or a compiled `re.Pattern[bytes]`.

`timeout_s` defaults to `session.timeout_s` (set in constructor).  When a
deadline is missed and `force_stop_on_timeout=True` (the default), `force_stop`
is called automatically before returning `False`.

#### Expect and mark pattern

Use `mark()` + `expect_from()` to assert ordering of serial events:

```python
session.expect(b"[kernel] kmain")

before_cmd = session.mark()
session.send("ps")
assert session.expect_from(before_cmd, b"pid=", timeout_s=10)
assert session.expect_from(before_cmd, b"wamos> ", timeout_s=10)
```

This is safer than a plain `expect` after a `send` because `expect` would
also match an earlier occurrence of the needle that was already in the buffer.

---

### QemuMonitor

`QemuMonitor` speaks the QEMU Machine Protocol (QMP) over a Unix-domain socket
(`AF_UNIX`, `SOCK_STREAM`).  QMP uses newline-delimited JSON — one object per
line — giving structured, machine-readable responses without text-parsing
fragility.

The `-qmp unix:<path>,server,wait=off` QEMU flag is added automatically by
`build_qemu_cmd` when `cfg.monitor_socket` is non-empty.  When
`cfg.enable_monitor=True` and no path is given, `QemuSession.start()` creates a
temporary directory under `/tmp` (kept short to respect macOS's 104-byte
`AF_UNIX` path limit), auto-generates the socket path, and cleans it up via
`atexit`.

#### QMP handshake

On `connect()`, the monitor:
1. Receives the QEMU greeting (`{"QMP": {"version": …}}`).
2. Sends `{"execute": "qmp_capabilities"}` to enter command mode.
3. Discards the `{"return": {}}` acknowledgement.

Connection retries with 0.1 s delays up to a configurable deadline (default
10 s; `QemuSession.start` uses 15 s).

#### Keyboard API

All keyboard methods use `input-send-event` with `{"type": "qcode"}` key
descriptors.  qcode names follow QEMU's `QKeyCode` enum in `qapi/ui.json`
(e.g. `"a"`, `"ret"`, `"ctrl"`, `"shift"`, `"esc"`, `"f1"`, `"backspace"`).

| Method                         | Description                                                  |
|--------------------------------|--------------------------------------------------------------|
| `key_down(key)`                | Send a key-press event (key stays held)                      |
| `key_up(key)`                  | Send a key-release event                                     |
| `key_press(key)`               | `key_down` then `key_up`                                     |
| `send_key(*keys, hold_ms=100)` | Hold a chord for `hold_ms` ms, then release in reverse order |
| `type_text(text, delay_s=0)`   | Inject ASCII text character by character via `_QCODE_MAP`    |

`type_text` uses the module-level `_QCODE_MAP` which covers the full
US-ASCII printable range (32–126) plus `\n`, `\t`, backspace (`\x08`), and ESC
(`\x1b`).  Each entry is a `(qcode_name, needs_shift)` pair.  Characters not
in the map are silently skipped.  The optional `delay_s` parameter inserts a
pause between keystrokes (default 0).

#### Mouse API

| Method                                              | Description                                                             |
|-----------------------------------------------------|-------------------------------------------------------------------------|
| `mouse_move_rel(dx, dy)`                            | Move relative to current position (pixels)                              |
| `mouse_move_abs(x, y, screen_w=1024, screen_h=768)` | Move to absolute screen coordinates, converted to QEMU's 0–0x7FFF range |
| `mouse_button_down(button)`                         | Press and hold a button (`"left"`, `"middle"`, `"right"`)               |
| `mouse_button_up(button)`                           | Release a button                                                        |
| `click(button)`                                     | Press then release                                                      |
| `double_click(button, delay_s=0.05)`                | Two clicks with an inter-click pause                                    |
| `scroll(clicks)`                                    | Positive = wheel-down, negative = wheel-up                              |

#### Status queries

| Method           | Returns                               |
|------------------|---------------------------------------|
| `query_status()` | `{"running": bool, "status": str, …}` |
| `query_mice()`   | List of mouse device dicts            |

#### HMP passthrough and raw execute

```python
# Human Monitor Protocol command via QMP passthrough
output = session.monitor.hmp("info pci")
output = session.monitor.hmp("sendkey ctrl-alt-delete")

# Raw QMP command with arbitrary arguments
resp = session.monitor.execute("query-block")

# Arguments with hyphens in their names (not valid Python identifiers)
resp = session.monitor.execute(
    "human-monitor-command",
    arguments={"command-line": "info registers"},
)
```

`execute()` discards async `{"event": …}` objects and raises `RuntimeError` on
QMP error responses.

#### Screenshots

```python
# Capture display as PNG (default) — no external tools required
path = session.monitor.screendump()           # auto-named under /tmp
path = session.monitor.screendump("/tmp/before_click.png")

# Raw PPM if PNG conversion is not wanted
path = session.monitor.screendump(fmt="ppm")
```

`screendump(path=None, fmt="png")` calls the HMP `screendump` command to
capture the VGA display as a PPM file, then converts it to PNG via
`_ppm_to_png()`.  The intermediate PPM is written to a temp file and deleted
after conversion.  `path` defaults to an auto-named file under `/tmp`.

`_ppm_to_png()` tries **Pillow** (`PIL.Image`) first for better compression;
if Pillow is not installed it falls back to a pure-Python implementation using
only `struct` and `zlib` from the standard library, so the feature works in a
plain Python environment with no third-party packages.

The PNG file can be read directly by image-processing tools or base64-encoded
for use with multimodal LLM APIs:

```python
import base64

path = session.monitor.screendump()
with open(path, "rb") as f:
    b64 = base64.b64encode(f.read()).decode()
# Pass b64 to your LLM client as an image_url or base64 image block.
```

**Constraint**: `screendump` captures the VGA framebuffer.  In `-nographic`
mode QEMU disables the display; the dump will be blank or return an HMP error.
A session needs `nographic=False` (plus a `display` backend or a virtual
framebuffer) for screenshots to contain meaningful content.

---

### Writing a Test

Integration tests live in `tests/test_*.py` and use Python's `unittest`
framework with `setUpClass`/`tearDownClass` to share one QEMU session across
all methods in a test case.

```python
import unittest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
from qemu_test_framework import QemuSession, default_config

class TestCliPs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        assert cls.session.expect(b"wamos> "), "boot timed out"

    @classmethod
    def tearDownClass(cls):
        cls.session.close()

    def test_ps_lists_processes(self):
        m = self.session.mark()
        self.session.send("ps")
        self.assertTrue(self.session.expect_from(m, b"pid=", timeout_s=10))
        self.assertTrue(self.session.expect_from(m, b"wamos> ", timeout_s=10))
```

#### Test with keyboard/mouse injection

```python
class TestKeyboardInput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cfg.enable_monitor = True        # enable QMP socket
        cfg.nographic = False            # required for graphical display
        cfg.display = "gtk"
        cls.session = QemuSession(cfg, timeout_s=120)
        cls.session.start()
        assert cls.session.expect(b"wamos> ")

    @classmethod
    def tearDownClass(cls):
        cls.session.close()

    def test_type_in_cli(self):
        m = self.session.mark()
        self.session.monitor.type_text("help\n")
        self.assertTrue(self.session.expect_from(m, b"commands:"))
```

---

### Standalone Scripts

| Script                                   | Purpose                                                    |
|------------------------------------------|------------------------------------------------------------|
| `scripts/qemu_halt_test.py`              | Boot + halt smoke gate (pre-commit)                        |
| `scripts/qemu_ring3_halt_test.py`        | Ring3 isolation + syscall marker assertions                |
| `scripts/qemu_ring3_fault_storm_test.py` | Scheduler liveness under repeated fault load               |
| `scripts/qemu_ui_test.py`                | UI + serial boot smoke with display backend auto-detection |
| `scripts/run_unittest_suite.py`          | `unittest` discovery runner for `tests/`                   |

---

### Constraints

**No parallel QEMU runs.**  All integration targets share `build/esp` (a mutable
FAT image).  Running two QEMU tests concurrently causes `Error deleting` races
and boot-config corruption.  Always run one target at a time.

**Tests assert runtime behaviour only.**  Serial output markers, exit codes,
and state transitions are valid.  Source-text presence checks (regex/string
matching against repository files) are forbidden — they are brittle and do not
verify behaviour.

**QMP monitor requires QEMU ≥ 2.x** for `input-send-event`.  All current
development environments meet this requirement.

**`type_text` uses the US-ASCII keyboard layout** (the `_QCODE_MAP` table is
built for a standard US keyboard).  Characters outside the printable ASCII range
or unmapped symbols are silently skipped.
