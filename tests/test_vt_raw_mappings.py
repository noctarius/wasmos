import os
import re
import unittest


class VtRawMappingSpecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        cls.vt_path = os.path.join(root, "src", "services", "vt", "vt_main.c")
        with open(cls.vt_path, "r", encoding="utf-8") as f:
            cls.src = f.read()

    def _require(self, pattern: str) -> None:
        self.assertRegex(
            self.src,
            re.compile(pattern, re.DOTALL),
            msg=f"Missing VT mapping pattern: {pattern}",
        )

    def test_arrow_mappings_exist(self):
        self._require(r"scancode\s*==\s*0x48[^\n]*\/\*\s*Up\s*\*/[\s\S]*?vt_input_q_push_escape\(tty,\s*'A'\)")
        self._require(r"scancode\s*==\s*0x50[^\n]*\/\*\s*Down\s*\*/[\s\S]*?vt_input_q_push_escape\(tty,\s*'B'\)")
        self._require(r"scancode\s*==\s*0x4D[^\n]*\/\*\s*Right\s*\*/[\s\S]*?vt_input_q_push_escape\(tty,\s*'C'\)")
        self._require(r"scancode\s*==\s*0x4B[^\n]*\/\*\s*Left\s*\*/[\s\S]*?vt_input_q_push_escape\(tty,\s*'D'\)")

    def test_nav_edit_mappings_exist(self):
        self._require(r"scancode\s*==\s*0x47[^\n]*\/\*\s*Home\s*\*/[\s\S]*?vt_input_q_push\(tty,\s*'H'\)")
        self._require(r"scancode\s*==\s*0x4F[^\n]*\/\*\s*End\s*\*/[\s\S]*?vt_input_q_push\(tty,\s*'F'\)")

        self._require(r"scancode\s*==\s*0x49[^\n]*\/\*\s*Page Up\s*\*/[\s\S]*?vt_input_q_push\(tty,\s*'5'\)[\s\S]*?vt_input_q_push\(tty,\s*'~'\)")
        self._require(r"scancode\s*==\s*0x51[^\n]*\/\*\s*Page Down\s*\*/[\s\S]*?vt_input_q_push\(tty,\s*'6'\)[\s\S]*?vt_input_q_push\(tty,\s*'~'\)")
        self._require(r"scancode\s*==\s*0x52[^\n]*\/\*\s*Insert\s*\*/[\s\S]*?vt_input_q_push\(tty,\s*'2'\)[\s\S]*?vt_input_q_push\(tty,\s*'~'\)")
        self._require(r"scancode\s*==\s*0x53[^\n]*\/\*\s*Delete\s*\*/[\s\S]*?vt_input_q_push\(tty,\s*'3'\)[\s\S]*?vt_input_q_push\(tty,\s*'~'\)")

    def test_shift_ascii_mapping_exists(self):
        self._require(r"static const uint8_t g_sc_to_ascii_shift\[58\]")
        self._require(r"g_shift_down\s*\?\s*g_sc_to_ascii_shift\[\(uint32_t\)scancode\]\s*:\s*g_sc_to_ascii\[\(uint32_t\)scancode\]")
        self._require(r"g_sc_to_ascii_shift\[58\][\s\S]*'\?'")

    def test_switch_replay_uses_reliable_send(self):
        self._require(r"static int\s+vt_fb_send_switch\(")
        self._require(r"vt_replay_tty\(tty_index,\s*1\)")
        self._require(r"vt_fb_send_switch\(FBTEXT_IPC_CLEAR_REQ")


if __name__ == "__main__":
    unittest.main()
