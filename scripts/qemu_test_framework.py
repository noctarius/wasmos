#!/usr/bin/env python3
import argparse
import atexit
import json
import os
import selectors
import socket
import struct
import subprocess
import zlib
import sys
import tempfile
import time
import shutil
from dataclasses import dataclass
from typing import Optional, Pattern, Union


@dataclass
class QemuConfig:
    ovmf_code: str
    ovmf_vars: str
    esp_dir: str
    userfs_dir: str = ""
    nographic: bool = True
    display: str = ""
    isolate_esp: bool = False
    # QMP monitor: set enable_monitor=True to have QemuSession auto-create the
    # socket, or set monitor_socket directly to a path to use a specific one.
    enable_monitor: bool = False
    monitor_socket: str = ""
    smp_count: int = 1
    # NIC model attached via user-mode networking so the virtio-net driver has a
    # device to probe.  Set to "none" (or WASMOS_QEMU_NIC_MODEL=none) to omit.
    nic_model: str = "virtio-net-pci"
    # QEMU netdev backend spec (everything after `-netdev`). Defaults to
    # user-mode (SLIRP) NAT with QEMU's built-in DHCP server. Override via the
    # WASMOS_QEMU_NETDEV env var to bridge onto a real network. Must keep
    # `id=net0` so the paired `-device ...,netdev=net0` still matches. Examples:
    #   macOS bridge to LAN:  vmnet-bridged,id=net0,ifname=en0   (needs sudo)
    #   macOS Apple NAT+DHCP:  vmnet-shared,id=net0              (needs sudo)
    #   Linux tap:             tap,id=net0,ifname=tap0,script=no,downscript=no
    netdev: str = "user,id=net0"

    def __post_init__(self) -> None:
        if self.userfs_dir:
            return
        env_userfs = os.environ.get("WASMOS_USERFS", "")
        if env_userfs:
            self.userfs_dir = env_userfs


def _read_cmake_cache(cache_path: str) -> dict:
    data = {}
    if not os.path.exists(cache_path):
        return data
    with open(cache_path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("//") or line.startswith("#"):
                continue
            if ":" not in line or "=" not in line:
                continue
            key, rest = line.split(":", 1)
            _, value = rest.split("=", 1)
            data[key] = value
    return data


def default_kernel_path(build_dir: str = "build") -> str:
    """Path to the kernel image the tests should boot.

    Honours WASMOS_KERNEL, which the CMake test targets set to their own
    ${BUILD_DIR}/kernel.elf. Without it every test resolved the literal "build"
    directory, so a suite launched from any other configuration's tree (a wasm3
    defconfig tree, say) copied the DEFAULT tree's kernel over its ESP and
    silently tested the wrong runtime -- while reporting a pass.
    """
    return os.environ.get("WASMOS_KERNEL", os.path.join(build_dir, "kernel.elf"))


CLI_PROMPT = b"wamos> "

# Multiplies every expect()/settle() deadline. A CI runner is far slower than a
# developer's machine -- boots that take 12s locally have taken over 120s there
# -- and raising each test's literal timeout would slow local runs for a problem
# they do not have. WASMOS_TEST_TIMEOUT_SCALE lets CI buy patience in one place.
_TIMEOUT_SCALE = float(os.environ.get("WASMOS_TEST_TIMEOUT_SCALE", "1") or "1")


def default_build_dir(build_dir: str = "build") -> str:
    """The build tree a test belongs to.

    Derived from WASMOS_KERNEL rather than exported separately, because every
    CMake test target already sets that to its own ${BUILD_DIR}/kernel.elf.
    """
    kernel = os.environ.get("WASMOS_KERNEL")
    if kernel:
        return os.path.dirname(kernel) or build_dir
    return build_dir


def default_host_tool_path(name: str, build_dir: str = "build") -> str:
    """Path to a host tool built by the tree under test.

    Same defect as default_kernel_path fixed, one tool later: a hardcoded
    "build/<tool>" makes a suite launched from a defconfig tree look for a
    binary that tree never built, and fail on a path rather than on behaviour.
    """
    return os.path.join(default_build_dir(build_dir), name)


def default_config(build_dir: str = "build") -> QemuConfig:
    cache = _read_cmake_cache(os.path.join(build_dir, "CMakeCache.txt"))
    ovmf_code = os.environ.get("WASMOS_OVMF_CODE", cache.get("OVMF_CODE", ""))
    ovmf_vars = os.environ.get("WASMOS_OVMF_VARS", cache.get("OVMF_VARS", ""))
    esp_dir = os.environ.get("WASMOS_ESP", os.path.join(build_dir, "esp"))
    source_dir = cache.get("CMAKE_HOME_DIRECTORY", os.getcwd())
    userfs_default = os.path.join(source_dir, "userfs")
    userfs_dir = os.environ.get("WASMOS_USERFS", userfs_default)
    isolate_esp = os.environ.get("WASMOS_QEMU_ISOLATE_ESP", "0") == "1"
    enable_monitor = os.environ.get("WASMOS_QEMU_MONITOR", "0") == "1"
    monitor_socket = os.environ.get("WASMOS_QEMU_MONITOR_SOCK", "")
    smp_count = int(os.environ.get("WASMOS_QEMU_SMP_COUNT", "1"))
    nic_model = os.environ.get(
        "WASMOS_QEMU_NIC_MODEL", cache.get("WASMOS_QEMU_NIC_MODEL", "virtio-net-pci")
    )
    netdev = os.environ.get(
        "WASMOS_QEMU_NETDEV", cache.get("WASMOS_QEMU_NETDEV", "user,id=net0")
    )
    if not ovmf_code:
        raise RuntimeError("OVMF_CODE not set (WASMOS_OVMF_CODE or CMakeCache.txt)")
    return QemuConfig(
        ovmf_code=ovmf_code,
        ovmf_vars=ovmf_vars,
        esp_dir=esp_dir,
        userfs_dir=userfs_dir,
        isolate_esp=isolate_esp,
        enable_monitor=enable_monitor,
        monitor_socket=monitor_socket,
        smp_count=smp_count,
        nic_model=nic_model,
        netdev=netdev,
    )


def build_qemu_cmd(cfg: QemuConfig) -> list:
    cmd = [
        "qemu-system-x86_64",
        "-m",
        "512M",
        "-cpu",
        "max",
    ]
    # Acceleration: prefer KVM when the host exposes a usable /dev/kvm (e.g. Linux
    # CI runners) so TCG emulation does not dominate boot time; otherwise fall back
    # to plain TCG. Override explicitly with WASMOS_QEMU_ACCEL (e.g. "hvf", "tcg").
    accel = os.environ.get("WASMOS_QEMU_ACCEL", "").strip()
    if not accel:
        accel = "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"
    if accel and accel != "tcg":
        cmd += ["-accel", accel]
    if cfg.smp_count > 1:
        cmd += ["-smp", str(cfg.smp_count)]
    cmd += [
        "-serial",
        "mon:stdio",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={cfg.ovmf_code}",
    ]
    if cfg.nographic:
        cmd += ["-nographic"]
    else:
        if cfg.display:
            cmd += ["-display", cfg.display]
    if cfg.ovmf_vars:
        cmd += ["-drive", f"if=pflash,format=raw,file={cfg.ovmf_vars}"]
    cmd += ["-drive", f"format=raw,file=fat:rw:{cfg.esp_dir}"]
    if cfg.userfs_dir:
        cmd += ["-drive", f"format=raw,file=fat:rw:{cfg.userfs_dir}"]
    if cfg.nic_model and cfg.nic_model != "none":
        # Give the NIC a stable device id so the monitor can target it with
        # `set_link nic0 on|off` (QemuSession.set_link) to exercise link events.
        # The netdev backend defaults to user-mode SLIRP but can be overridden
        # (e.g. vmnet-bridged) via WASMOS_QEMU_NETDEV; it must keep id=net0.
        cmd += [
            "-netdev",
            cfg.netdev,
            "-device",
            f"{cfg.nic_model},netdev=net0,id=nic0",
        ]
    # Entropy source for the virtio-rng driver (transitional 1AF4:1005).
    cmd += ["-device", "virtio-rng-pci"]
    if cfg.monitor_socket:
        cmd += ["-qmp", f"unix:{cfg.monitor_socket},server,wait=off"]
    return cmd


# Maps ASCII characters to (qcode_name, needs_shift) for QMP input-send-event.
# qcode names follow QEMU's QKeyCode enum (qapi/ui.json).
_QCODE_MAP: dict = {
    " ": ("spc", False),
    "!": ("1", True),
    '"': ("apostrophe", True),
    "#": ("3", True),
    "$": ("4", True),
    "%": ("5", True),
    "&": ("7", True),
    "'": ("apostrophe", False),
    "(": ("9", True),
    ")": ("0", True),
    "*": ("8", True),
    "+": ("equal", True),
    ",": ("comma", False),
    "-": ("minus", False),
    ".": ("dot", False),
    "/": ("slash", False),
    ":": ("semicolon", True),
    ";": ("semicolon", False),
    "<": ("comma", True),
    "=": ("equal", False),
    ">": ("dot", True),
    "?": ("slash", True),
    "@": ("2", True),
    "[": ("bracket_left", False),
    "\\": ("backslash", False),
    "]": ("bracket_right", False),
    "^": ("6", True),
    "_": ("minus", True),
    "`": ("grave_accent", False),
    "{": ("bracket_left", True),
    "|": ("backslash", True),
    "}": ("bracket_right", True),
    "~": ("grave_accent", True),
    "\n": ("ret", False),
    "\t": ("tab", False),
    "\x08": ("backspace", False),
    "\x1b": ("esc", False),
    **{d: (d, False) for d in "0123456789"},
    **{c: (c, False) for c in "abcdefghijklmnopqrstuvwxyz"},
    **{c: (c.lower(), True) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
}


def _ppm_to_png(ppm_path: str, png_path: str) -> None:
    """Convert a P6 PPM file to PNG.

    Tries Pillow (PIL) first for better compression; falls back to a
    pure-Python implementation using only stdlib struct + zlib.
    """
    try:
        from PIL import Image as _Image

        _Image.open(ppm_path).save(png_path)
        return
    except ImportError:
        pass

    with open(ppm_path, "rb") as fh:
        # Read the three required header tokens, skipping comment lines.
        tokens: list = []
        while len(tokens) < 3:
            line = fh.readline().strip()
            if line and not line.startswith(b"#"):
                tokens.append(line)
        if tokens[0] != b"P6":
            raise ValueError(f"not a P6 PPM file: {ppm_path!r}")
        w, h = map(int, tokens[1].split())
        maxval = int(tokens[2])
        raw = fh.read()

    if maxval != 255:
        raise ValueError(f"unsupported PPM maxval {maxval}: only 255 is handled")

    def _chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (
            struct.pack(">I", len(data))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    stride = w * 3
    # Prepend filter-type byte 0 (None) to each scanline, then zlib-compress.
    filtered = b"".join(b"\x00" + raw[y * stride : (y + 1) * stride] for y in range(h))

    with open(png_path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(_chunk(b"IDAT", zlib.compress(filtered, 6)))
        fh.write(_chunk(b"IEND", b""))


class QemuMonitor:
    """QMP client for QEMU monitor control (keyboard, mouse, status queries).

    Uses QEMU Machine Protocol (QMP) over a Unix-domain socket.  The preferred
    way to obtain an instance is via QemuSession.monitor after starting a
    session with QemuConfig.enable_monitor=True.

    Keyboard injection uses QMP qcode names (e.g. 'a', 'ret', 'ctrl', 'esc').
    Mouse injection supports both relative and absolute movement plus buttons.
    Any HMP command not covered by the typed methods is reachable via hmp().
    """

    def __init__(self, socket_path: str, timeout_s: float = 5.0) -> None:
        self.socket_path = socket_path
        self.timeout_s = timeout_s
        self._sock: Optional[socket.socket] = None
        self._buf = b""

    # --- Connection lifecycle ---

    def connect(self, timeout_s: float = 10.0) -> None:
        """Connect to the QMP socket and negotiate capabilities.

        Retries with short delays until the socket is ready or the deadline
        is reached.  QEMU typically makes the socket available within a few
        hundred milliseconds of launch.
        """
        deadline = time.time() + timeout_s
        last_exc: Optional[Exception] = None
        while time.time() < deadline:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(self.timeout_s)
                sock.connect(self.socket_path)
                self._sock = sock
                self._buf = b""
                self._read_object()  # discard QMP greeting {"QMP": ...}
                self._send_object({"execute": "qmp_capabilities"})
                self._read_object()  # discard {"return": {}}
                return
            except (ConnectionRefusedError, FileNotFoundError, OSError) as exc:
                last_exc = exc
                if self._sock:
                    try:
                        self._sock.close()
                    except Exception:
                        pass
                    self._sock = None
                time.sleep(0.1)
        raise RuntimeError(
            f"QemuMonitor: cannot connect to {self.socket_path!r}: {last_exc}"
        )

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None

    def __enter__(self) -> "QemuMonitor":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    # --- QMP wire protocol ---

    def _send_object(self, obj: dict) -> None:
        if not self._sock:
            raise RuntimeError("QemuMonitor: not connected")
        self._sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))

    def _read_object(self) -> dict:
        """Read the next newline-delimited JSON object from the QMP stream."""
        while True:
            idx = self._buf.find(b"\n")
            if idx >= 0:
                line = self._buf[:idx].strip()
                self._buf = self._buf[idx + 1 :]
                if line:
                    return json.loads(line.decode("utf-8"))
                continue
            if not self._sock:
                raise RuntimeError("QemuMonitor: connection closed")
            chunk = self._sock.recv(4096)
            if not chunk:
                raise RuntimeError("QemuMonitor: server closed connection")
            self._buf += chunk

    def execute(self, cmd: str, arguments: Optional[dict] = None, **kwargs) -> dict:
        """Execute a QMP command and return the response dict.

        arguments: optional dict for QMP argument names that contain hyphens
                   and cannot be expressed as Python keyword arguments.
        kwargs: additional arguments merged into the arguments dict.

        Async event objects ({"event": ...}) are silently discarded while
        waiting for the command reply.  Raises RuntimeError on QMP errors.
        """
        msg: dict = {"execute": cmd}
        args: dict = {}
        if arguments:
            args.update(arguments)
        if kwargs:
            args.update(kwargs)
        if args:
            msg["arguments"] = args
        self._send_object(msg)
        while True:
            resp = self._read_object()
            if "return" in resp:
                return resp
            if "error" in resp:
                desc = resp["error"].get("desc", str(resp["error"]))
                raise RuntimeError(f"QMP error for {cmd!r}: {desc}")
            # discard {"event": ...} async notifications

    def hmp(self, cmd: str) -> str:
        """Execute an HMP command via the QMP human-monitor-command passthrough.

        Returns the text output produced by the HMP command (may be empty).
        Use this for monitor commands not exposed by the typed API, e.g.:
            session.monitor.hmp("info pci")
            session.monitor.hmp("sendkey ctrl-alt-delete")
        """
        self._send_object(
            {
                "execute": "human-monitor-command",
                "arguments": {"command-line": cmd},
            }
        )
        while True:
            resp = self._read_object()
            if "return" in resp:
                return resp["return"]
            if "error" in resp:
                desc = resp["error"].get("desc", str(resp["error"]))
                raise RuntimeError(f"QemuMonitor hmp error: {desc}")

    # --- Keyboard ---

    def key_down(self, key: str) -> None:
        """Send a key-press event (key stays held until key_up).

        key must be a QMP qcode name, e.g. 'a', 'ret', 'ctrl', 'shift', 'esc',
        'f1', 'left', 'up', 'home', 'pgup', 'tab', 'backspace', 'spc'.
        """
        self.execute(
            "input-send-event",
            events=[
                {
                    "type": "key",
                    "data": {"down": True, "key": {"type": "qcode", "data": key}},
                }
            ],
        )

    def key_up(self, key: str) -> None:
        """Send a key-release event."""
        self.execute(
            "input-send-event",
            events=[
                {
                    "type": "key",
                    "data": {"down": False, "key": {"type": "qcode", "data": key}},
                }
            ],
        )

    def key_press(self, key: str) -> None:
        """Send a key-down then key-up for a single key."""
        self.key_down(key)
        self.key_up(key)

    def send_key(self, *keys: str, hold_ms: int = 100) -> None:
        """Hold a key chord for hold_ms milliseconds then release all keys.

        Keys are pressed in order and released in reverse order, matching
        how a user would type a chord.  Example:
            session.monitor.send_key('ctrl', 'alt', 'delete')
        """
        for k in keys:
            self.key_down(k)
        time.sleep(hold_ms / 1000.0)
        for k in reversed(keys):
            self.key_up(k)

    def type_text(self, text: str, delay_s: float = 0.0) -> None:
        """Inject ASCII text as keyboard events using the built-in qcode map.

        Unsupported characters (non-ASCII or unmapped) are silently skipped.
        delay_s: optional pause between each keystroke (default 0).
        """
        for ch in text:
            entry = _QCODE_MAP.get(ch)
            if entry is None:
                continue
            qcode, needs_shift = entry
            if needs_shift:
                self.key_down("shift")
            self.key_press(qcode)
            if needs_shift:
                self.key_up("shift")
            if delay_s > 0.0:
                time.sleep(delay_s)

    # --- Mouse ---

    def mouse_move_rel(self, dx: int, dy: int) -> None:
        """Move the mouse pointer by (dx, dy) pixels relative to its current position."""
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        if events:
            self.execute("input-send-event", events=events)

    def mouse_move_abs(
        self, x: int, y: int, screen_w: int = 1024, screen_h: int = 768
    ) -> None:
        """Move the mouse pointer to an absolute screen position.

        x and y are pixel coordinates within a screen_w × screen_h display.
        They are converted to QEMU's internal 0–0x7FFF range.
        """
        ax = int(x * 0x7FFF / max(screen_w - 1, 1))
        ay = int(y * 0x7FFF / max(screen_h - 1, 1))
        self.execute(
            "input-send-event",
            events=[
                {
                    "type": "abs",
                    "data": {"axis": "x", "value": max(0, min(0x7FFF, ax))},
                },
                {
                    "type": "abs",
                    "data": {"axis": "y", "value": max(0, min(0x7FFF, ay))},
                },
            ],
        )

    def mouse_button_down(self, button: str = "left") -> None:
        """Press a mouse button without releasing it.

        button: 'left', 'middle', 'right', 'wheel-up', or 'wheel-down'.
        """
        self.execute(
            "input-send-event",
            events=[{"type": "btn", "data": {"down": True, "button": button}}],
        )

    def mouse_button_up(self, button: str = "left") -> None:
        """Release a mouse button."""
        self.execute(
            "input-send-event",
            events=[{"type": "btn", "data": {"down": False, "button": button}}],
        )

    def click(self, button: str = "left") -> None:
        """Click a mouse button (press then release)."""
        self.mouse_button_down(button)
        self.mouse_button_up(button)

    def double_click(self, button: str = "left", delay_s: float = 0.05) -> None:
        """Double-click a mouse button with an inter-click pause."""
        self.click(button)
        time.sleep(delay_s)
        self.click(button)

    def scroll(self, clicks: int) -> None:
        """Scroll the mouse wheel.  Positive clicks scroll down, negative up."""
        button = "wheel-down" if clicks > 0 else "wheel-up"
        for _ in range(abs(clicks)):
            self.click(button)

    # --- Status queries ---

    def query_status(self) -> dict:
        """Return the QEMU VM running status dict (e.g. {'running': True, ...})."""
        return self.execute("query-status").get("return", {})

    def query_mice(self) -> list:
        """Return the list of input mouse devices known to QEMU."""
        return self.execute("query-mice").get("return", [])

    # --- Screenshots ---

    def screendump(self, path: Optional[str] = None, fmt: str = "png") -> str:
        """Capture a screenshot of the QEMU graphical display.

        path: destination file path. If None, a temp file is created under /tmp.
        fmt:  'png' (default) or 'ppm'.  PNG is converted from QEMU's native
              PPM output using _ppm_to_png(), which tries Pillow first and
              falls back to a pure-Python stdlib implementation.

        Returns the path of the saved image file.

        Note: screendump captures the VGA display.  In -nographic mode the
        display is disabled and the dump will be blank or return an HMP error.
        """
        if fmt not in ("png", "ppm"):
            raise ValueError(f"unsupported fmt {fmt!r}: choose 'png' or 'ppm'")

        if path is None:
            fd, path = tempfile.mkstemp(
                suffix=f".{fmt}", prefix="wasmos-screen-", dir="/tmp"
            )
            os.close(fd)

        if fmt == "ppm":
            self.hmp(f"screendump {path}")
            return path

        # Dump native PPM to a temp file, convert to PNG, clean up the PPM.
        fd, ppm_path = tempfile.mkstemp(
            suffix=".ppm", prefix="wasmos-screen-", dir="/tmp"
        )
        os.close(fd)
        try:
            self.hmp(f"screendump {ppm_path}")
            _ppm_to_png(ppm_path, path)
        finally:
            try:
                os.unlink(ppm_path)
            except OSError:
                pass
        return path


class QemuSession:
    def __init__(
        self,
        cfg: QemuConfig,
        timeout_s: int = 120,
        echo: bool = True,
        force_stop_on_timeout: bool = True,
    ):
        self.cfg = cfg
        self.timeout_s = timeout_s
        self.echo = echo
        self.force_stop_on_timeout = force_stop_on_timeout
        self.proc: Optional[subprocess.Popen] = None
        self.selector: Optional[selectors.BaseSelector] = None
        self.buf = b""
        self._esp_runtime_dir: Optional[str] = None
        self.monitor: Optional[QemuMonitor] = None
        self._monitor_tmp_dir: Optional[str] = None
        # Cleared until the first CLI prompt has been waited out; see _cli_ready.
        self._cli_settled = False

    def _cleanup_esp_runtime_dir(self) -> None:
        if not self._esp_runtime_dir:
            return
        shutil.rmtree(self._esp_runtime_dir, ignore_errors=True)
        self._esp_runtime_dir = None

    def _cleanup_monitor_tmp_dir(self) -> None:
        if not self._monitor_tmp_dir:
            return
        shutil.rmtree(self._monitor_tmp_dir, ignore_errors=True)
        self._monitor_tmp_dir = None

    def start(self) -> None:
        if not os.path.exists(self.cfg.ovmf_code):
            raise FileNotFoundError(f"OVMF code not found: {self.cfg.ovmf_code}")
        if self.cfg.ovmf_vars and not os.path.exists(self.cfg.ovmf_vars):
            raise FileNotFoundError(f"OVMF vars not found: {self.cfg.ovmf_vars}")
        if not os.path.isdir(self.cfg.esp_dir):
            raise FileNotFoundError(f"ESP dir not found: {self.cfg.esp_dir}")

        runtime_cfg = self.cfg
        if self.cfg.isolate_esp:
            temp_root = tempfile.mkdtemp(prefix="wasmos-esp-")
            runtime_esp = os.path.join(temp_root, "esp")
            shutil.copytree(self.cfg.esp_dir, runtime_esp)
            runtime_cfg = QemuConfig(
                ovmf_code=self.cfg.ovmf_code,
                ovmf_vars=self.cfg.ovmf_vars,
                esp_dir=runtime_esp,
                userfs_dir=self.cfg.userfs_dir,
                nographic=self.cfg.nographic,
                display=self.cfg.display,
                isolate_esp=False,
                enable_monitor=self.cfg.enable_monitor,
                monitor_socket=self.cfg.monitor_socket,
                smp_count=self.cfg.smp_count,
                nic_model=self.cfg.nic_model,
                netdev=self.cfg.netdev,
            )
            self._esp_runtime_dir = temp_root
            atexit.register(self._cleanup_esp_runtime_dir)

        # Auto-generate a QMP socket path when monitor is enabled but no path given.
        # /tmp is used to keep the path short (Unix socket path limit is 104 bytes on
        # macOS) and avoid leaving sockets in unexpected locations.
        monitor_socket = runtime_cfg.monitor_socket
        if runtime_cfg.enable_monitor and not monitor_socket:
            tmp_dir = tempfile.mkdtemp(prefix="wasmos-qmp-", dir="/tmp")
            self._monitor_tmp_dir = tmp_dir
            atexit.register(self._cleanup_monitor_tmp_dir)
            monitor_socket = os.path.join(tmp_dir, "qmp.sock")
            runtime_cfg = QemuConfig(
                ovmf_code=runtime_cfg.ovmf_code,
                ovmf_vars=runtime_cfg.ovmf_vars,
                esp_dir=runtime_cfg.esp_dir,
                userfs_dir=runtime_cfg.userfs_dir,
                nographic=runtime_cfg.nographic,
                display=runtime_cfg.display,
                isolate_esp=False,
                enable_monitor=True,
                monitor_socket=monitor_socket,
                smp_count=runtime_cfg.smp_count,
                nic_model=runtime_cfg.nic_model,
                netdev=runtime_cfg.netdev,
            )

        cmd = build_qemu_cmd(runtime_cfg)
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.buf = b""

        if monitor_socket:
            self.monitor = QemuMonitor(monitor_socket)
            self.monitor.connect(timeout_s=15.0)

    def close(self) -> None:
        if self.monitor:
            try:
                self.monitor.close()
            except Exception:
                pass
            self.monitor = None

        if not self.proc:
            return
        try:
            if self.proc.poll() is None:
                try:
                    self.force_stop()
                    self.proc.terminate()
                    self.proc.wait(timeout=5)
                except Exception:
                    self.proc.kill()
        finally:
            if self.selector:
                try:
                    if self.proc and self.proc.stdout:
                        try:
                            self.selector.unregister(self.proc.stdout)
                        except Exception:
                            pass
                    self.selector.close()
                finally:
                    self.selector = None

            if self.proc:
                if self.proc.stdin:
                    try:
                        self.proc.stdin.close()
                    except Exception:
                        pass
                if self.proc.stdout:
                    try:
                        self.proc.stdout.close()
                    except Exception:
                        pass
            self.proc = None
            self._cleanup_esp_runtime_dir()

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    def send(self, line: str) -> None:
        if not self.proc or not self.proc.stdin:
            raise RuntimeError("QEMU process not running")
        if self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write(line.encode("utf-8") + b"\r\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            return

    def set_link(self, up: bool, nic: str = "nic0") -> str:
        """Force the emulated NIC link up or down via the QEMU monitor.

        Requires the session to have been started with a monitor
        (QemuConfig.enable_monitor=True). QEMU flips the virtio-net
        VIRTIO_NET_S_LINK_UP config-status bit, which the driver polls and
        forwards to net-stack as NETDRV_IPC_LINK_NOTIFY. Returns the raw HMP
        output (normally empty on success).
        """
        if self.monitor is None:
            raise RuntimeError(
                "set_link requires a monitor; start with enable_monitor=True"
            )
        return self.monitor.hmp(f"set_link {nic} {'on' if up else 'off'}")

    def force_stop(self) -> None:
        if not self.proc or not self.proc.stdin:
            return
        if self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write(b"\x01x")
            self.proc.stdin.flush()
        except Exception:
            return

    def _pump(self, timeout_s: float) -> None:
        if not self.selector or not self.proc:
            return
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return
            events = self.selector.select(timeout=0.1)
            for key, _ in events:
                chunk = key.fileobj.read(4096)
                if not chunk:
                    continue
                if self.echo:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                self.buf += chunk

    def expect(
        self, needle: Union[bytes, str, Pattern[bytes]], timeout_s: Optional[int] = None
    ) -> bool:
        if isinstance(needle, str):
            needle_b = needle.encode("utf-8")
            pattern = None
        elif isinstance(needle, bytes):
            needle_b = needle
            pattern = None
        else:
            needle_b = b""
            pattern = needle

        limit = timeout_s if timeout_s is not None else self.timeout_s
        deadline = time.time() + limit * _TIMEOUT_SCALE
        while time.time() < deadline:
            if pattern:
                if pattern.search(self.buf):
                    return self._cli_ready(needle_b)
            else:
                if needle_b in self.buf:
                    return self._cli_ready(needle_b)
            self._pump(0.2)
        if self.force_stop_on_timeout:
            self.force_stop()
        return False

    def _cli_ready(self, needle: bytes) -> bool:
        """Hook on a successful expect(): the FIRST CLI prompt is not readiness.

        The prompt appears as soon as the shell is up, while services behind it
        are still starting. A test that begins driving the console there races
        the rest of boot, and on a loaded machine the remaining startup work
        starves the CLI past a per-command timeout -- surfacing as "prompt not
        found after 'ls'" when nothing was wrong with ls.

        Rather than have every test remember to settle, the primitive stops
        reporting the prompt as found until the console is usable. Only the first
        match settles: later prompts are a command completing, where waiting for
        silence would be wrong as well as slow.
        """
        if self._cli_settled or CLI_PROMPT not in needle:
            return True
        self._cli_settled = True
        return self.settle()

    def mark(self) -> int:
        return len(self.buf)

    def expect_from(
        self,
        start: int,
        needle: Union[bytes, str, Pattern[bytes]],
        timeout_s: Optional[int] = None,
    ) -> bool:
        """Wait for `needle` in the output produced after `start`.

        Unlike expect(), a timeout here never stops the VM. The two answer
        different questions: expect() waits for a session-level milestone, so a
        timeout there means the session is unusable and killing it is right;
        expect_from() waits for one command's output within a session the caller
        already has, and every caller reports that timeout itself -- often as a
        retry (`if expect_from(...): break`), where a miss is the normal case.

        Stopping the VM here punished the whole test class for one command,
        because the session is created in setUpClass and shared: a single
        missing prompt after `ls` became 14 failures and 13 minutes of timing
        out against a dead machine (CI run 31889615410). Sessions are torn down
        explicitly in tearDownClass, so nothing depended on this kill.
        """
        if isinstance(needle, str):
            needle_b = needle.encode("utf-8")
            pattern = None
        elif isinstance(needle, bytes):
            needle_b = needle
            pattern = None
        else:
            needle_b = b""
            pattern = needle

        limit = timeout_s if timeout_s is not None else self.timeout_s
        deadline = time.time() + limit * _TIMEOUT_SCALE
        while time.time() < deadline:
            if start < 0:
                start = 0
            view = self.buf[start:]
            if pattern:
                if pattern.search(view):
                    return True
            else:
                if needle_b in view:
                    return True
            self._pump(0.2)
        return False

    def settle(self, probe_s: int = 3, timeout_s: int = 30) -> bool:
        """Wait until the CLI answers promptly, i.e. it is usable.

        Readiness is a PROBE, not silence. An earlier version of this waited for
        the console to go quiet first, which is wrong twice: a system running the
        gfx demos or a vt never goes quiet, so it burned its whole budget and
        made every battery minutes slower; and startup has natural gaps, so on a
        quiet system the window lands in one and proves nothing.

        Sending an empty line and requiring the prompt back within probe_s tests
        exactly the property a caller needs -- that a command completes quickly --
        and is unaffected by whatever else is printing.

        An unanswered probe is the condition being waited out, not a dead VM,
        which is why the probes go through expect_from -- it does not stop the
        machine on a timeout.

        Returns True once a probe is answered, False if none is within timeout_s.
        """
        deadline = time.time() + timeout_s * _TIMEOUT_SCALE
        while time.time() < deadline:
            mark = self.mark()
            self.send("")
            if self.expect_from(mark, CLI_PROMPT, timeout_s=probe_s):
                return True
        return False

    def tail(self, max_bytes: int = 2048) -> str:
        if max_bytes <= 0:
            return ""
        data = self.buf[-max_bytes:]
        return data.decode("utf-8", errors="replace")

    def wait_for_exit(self, timeout_s: float = 5.0) -> bool:
        if not self.proc:
            return True
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return True
            self._pump(0.2)
        return self.proc.poll() is not None


def main():
    parser = argparse.ArgumentParser(description="QEMU test framework smoke run.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--userfs", default="")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--smp", type=int, default=1)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        userfs = args.userfs or os.environ.get(
            "WASMOS_USERFS", os.path.join(os.getcwd(), "userfs")
        )
        cfg = QemuConfig(
            args.ovmf_code, args.ovmf_vars, args.esp, userfs, smp_count=args.smp
        )
    else:
        cfg = default_config()

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        if not session.expect(b"wamos> "):
            return 1
        session.send("halt")
        if not session.wait_for_exit(5):
            sys.stderr.write("FAIL: halt did not power off QEMU\n")
            return 1
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
