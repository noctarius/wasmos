"""GFX_IPC_GET_WINDOW_TITLE round-trips through an app-owned transfer buffer.

menu_bar is spawned by scripts/system/sysinit.rc on every boot and is the only
caller of GFX_IPC_GET_WINDOW_TITLE in the tree. It owns the destination buffer,
borrows it WRITE to the compositor, and reads the reply straight out of its own
mapping -- the ownership shape docs/architecture/12-dma-transfers.md requires,
where the client of an exchange owns the buffer and the server is a transient
grantee.

The failure this guards is SILENT. handle_get_window_title replies
WASMOS_ERR_NONE with a length whenever the buffer id or borrow id is rejected,
so a broken exchange produces a perfectly healthy boot with an empty menu: no
panic, no error line, and a screenshot that still shows a desktop because the
compositor draws window title bars itself. Only the fetched byte count
distinguishes the two, which is what menu_bar reports once per boot and this
asserts.

Serial-only on purpose. A mouse-driven variant that opens the Apps menu and
screenshots it is better evidence for a human, but input injection needs
WASMOS_TEST_INPUT_INJECTION=1 and a real display backend (see
test_libui_click.py), so it skips exactly where CI runs.
"""

import os
import re
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class MenuBarTitleTest(unittest.TestCase):
    session = None

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=200, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=180):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_a_window_title_reaches_the_client_through_its_own_mapping(self):
        # menu_bar fetches lazily, on the layout pass that lists windows, so the
        # marker can arrive after the prompt. The windows sysinit starts (gfx
        # smoke, the libui demo) are what give it a title to ask for.
        # A non-zero count in one pattern: bytes=0 would mean the exchange
        # completed and copied nothing, which is the silent failure. Asserted
        # positively rather than as "not bytes=0", because a negative expect()
        # has to wait out its own timeout and trips the stall-dump machinery on
        # every healthy run.
        self.assertTrue(
            self.session.expect(
                re.compile(rb"\[test\] menu bar title bytes=[1-9][0-9]* first=."),
                timeout_s=60,
            ),
            "no window title round-tripped with a non-zero length. The "
            "compositor writes into a buffer the app owns and lent it WRITE; a "
            "rejected buffer_id or borrow_id returns success with no bytes, so "
            "the desktop looks correct and the menu is empty.\n--- tail ---\n"
            f"{self.session.tail()}\n",
        )


if __name__ == "__main__":
    unittest.main()
