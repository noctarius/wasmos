import os
import re
import unittest


class MemoryPrivilegeFoundationSpecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        cls.cpu_path = os.path.join(root, "src", "kernel", "cpu.c")
        cls.isr_path = os.path.join(root, "src", "kernel", "arch", "x86_64", "cpu_isr.S")
        cls.paging_path = os.path.join(root, "src", "kernel", "paging.c")
        cls.wasm3_link_path = os.path.join(root, "src", "kernel", "wasm3_link.c")
        with open(cls.cpu_path, "r", encoding="utf-8") as f:
            cls.cpu_src = f.read()
        with open(cls.isr_path, "r", encoding="utf-8") as f:
            cls.isr_src = f.read()
        with open(cls.paging_path, "r", encoding="utf-8") as f:
            cls.paging_src = f.read()
        with open(cls.wasm3_link_path, "r", encoding="utf-8") as f:
            cls.wasm3_link_src = f.read()

    def _require(self, src: str, pattern: str, msg: str) -> None:
        self.assertRegex(src, re.compile(pattern, re.DOTALL), msg=msg)

    def test_syscall_boundary_wiring_exists(self):
        self._require(
            self.cpu_src,
            r"X86_VECTOR_SYSCALL",
            "cpu.c should reference syscall vector constant",
        )
        self._require(
            self.cpu_src,
            r"idt_set_gate\(\(uint8_t\)X86_VECTOR_SYSCALL,\s*\(uintptr_t\)isr_syscall_128,\s*IDT_TYPE_INTERRUPT_GATE_USER\)",
            "cpu.c should install a user-callable syscall gate",
        )
        self._require(
            self.isr_src,
            r"\.global isr_syscall_128[\s\S]*call x86_syscall_handler",
            "ISR assembly should define syscall handler stub",
        )

    def test_user_mapping_flag_is_plumbed(self):
        self._require(
            self.paging_src,
            r"PT_FLAG_USER",
            "paging.c should define user page-table flag",
        )
        self._require(
            self.paging_src,
            r"if \(flags & MEM_REGION_FLAG_USER\)[\s\S]*map_flags \|= PT_FLAG_USER",
            "paging.c should map user-flagged regions with PT_FLAG_USER",
        )

    def test_io_hostcalls_check_capability(self):
        self._require(
            self.wasm3_link_src,
            r"require_io_capability",
            "wasm3_link.c should define io capability requirement helper",
        )
        self._require(
            self.wasm3_link_src,
            r"wasmos_io_in8[\s\S]*require_io_capability",
            "wasmos_io_in8 should enforce io capability",
        )
        self._require(
            self.wasm3_link_src,
            r"wasmos_io_out8[\s\S]*require_io_capability",
            "wasmos_io_out8 should enforce io capability",
        )


if __name__ == "__main__":
    unittest.main()
