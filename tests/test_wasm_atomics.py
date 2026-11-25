import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class WasmAtomicsTest(unittest.TestCase):
    """Both WASM runtimes execute the threads proposal's atomic loads and stores.

    Guests publish ring entries and device descriptors with atomic accesses:
    in the wasm memory model they are sequentially consistent, which is what
    orders a guest's plain stores before the index that publishes them on a
    weakly ordered CPU. examples/c/atomics_smoke is compiled with -matomics and
    round-trips a value through i32.atomic.store/.store8/.store16 and
    i32.atomic.load/.load8_u/.load16_u; a 4-byte atomic access to an address
    that is not 4-byte aligned must trap.

    Regression: 2026-11-24-wasm-guest-atomics. Neither runtime decoded the 0xFE
    prefix, so guests were built without atomics and every acquire, release
    and fence in guest code compiled to a plain access -- unordered on aarch64.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=180, echo=True)
        cls.session.start()
        cls.session.require_prompt()
        if not cls.session.settle():
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(
                f"system never went idle after boot\n--- tail ---\n{tail}\n"
            )

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_atomic_loads_and_stores_round_trip(self) -> None:
        mark = self.session.send_command("spawn /boot/apps/atomics_smoke", timeout_s=30)
        self.assertTrue(
            self.session.expect_from(mark, b"[test] atomics ok", timeout_s=30),
            f"atomics_smoke did not complete\n--- tail ---\n{self.session.tail()}",
        )

    def test_misaligned_atomic_traps(self) -> None:
        mark = self.session.send_command(
            "spawn /boot/apps/atomics_smoke misaligned", timeout_s=30
        )
        # The trap ends the app's entry with a failure, whichever runtime ran it
        # (wasm3 names the trap "[trap] unaligned atomic").
        self.assertTrue(
            self.session.expect_from(mark, b"[pm] app entry failed", timeout_s=30),
            f"the misaligned atomic load did not end the app\n--- tail ---\n"
            f"{self.session.tail()}",
        )
        self.assertNotIn(
            b"[test] atomics misaligned load did not trap",
            bytes(self.session.buf[mark:]),
            "a misaligned 4-byte atomic load did not trap",
        )
