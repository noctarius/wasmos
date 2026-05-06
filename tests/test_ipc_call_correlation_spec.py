import re
import unittest
from pathlib import Path


class IpcCallCorrelationSpecTest(unittest.TestCase):
    def setUp(self):
        self.src = Path("src/kernel/syscall.c").read_text(encoding="utf-8")

    def test_pending_queue_structure_exists(self):
        self.assertRegex(self.src, r"SYSCALL_IPC_PENDING_DEPTH")
        self.assertRegex(self.src, r"ipc_message_t\s+pending\[SYSCALL_IPC_PENDING_DEPTH\]")
        self.assertRegex(self.src, r"pending_head")
        self.assertRegex(self.src, r"pending_count")

    def test_unmatched_replies_are_preserved(self):
        self.assertRegex(self.src, r"syscall_ipc_pending_enqueue\s*\(")
        self.assertRegex(self.src, r"resp\.request_id\s*!=\s*request_id[\s\S]*pending_enqueue")

    def test_pending_queue_scanned_before_blocking_recv(self):
        self.assertRegex(self.src, r"syscall_ipc_pending_take_request\s*\(slot,\s*request_id,\s*&resp\)")


if __name__ == "__main__":
    unittest.main()
