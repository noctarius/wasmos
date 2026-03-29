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
        cls.process_h_path = os.path.join(root, "src", "kernel", "include", "process.h")
        cls.process_c_path = os.path.join(root, "src", "kernel", "process.c")
        cls.ctxsw_path = os.path.join(root, "src", "kernel", "arch", "x86_64", "context_switch.S")
        cls.kernel_c_path = os.path.join(root, "src", "kernel", "kernel.c")
        cls.syscall_c_path = os.path.join(root, "src", "kernel", "syscall.c")
        with open(cls.cpu_path, "r", encoding="utf-8") as f:
            cls.cpu_src = f.read()
        with open(cls.isr_path, "r", encoding="utf-8") as f:
            cls.isr_src = f.read()
        with open(cls.paging_path, "r", encoding="utf-8") as f:
            cls.paging_src = f.read()
        with open(cls.wasm3_link_path, "r", encoding="utf-8") as f:
            cls.wasm3_link_src = f.read()
        with open(cls.process_h_path, "r", encoding="utf-8") as f:
            cls.process_h_src = f.read()
        with open(cls.process_c_path, "r", encoding="utf-8") as f:
            cls.process_c_src = f.read()
        with open(cls.ctxsw_path, "r", encoding="utf-8") as f:
            cls.ctxsw_src = f.read()
        with open(cls.kernel_c_path, "r", encoding="utf-8") as f:
            cls.kernel_c_src = f.read()
        with open(cls.syscall_c_path, "r", encoding="utf-8") as f:
            cls.syscall_c_src = f.read()

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

    def test_ring3_context_restore_scaffolding_exists(self):
        self._require(
            self.process_h_src,
            r"uint64_t cs;\s*uint64_t ss;\s*uint64_t user_rsp;",
            "process context should carry cs/ss/user_rsp fields",
        )
        self._require(
            self.process_c_src,
            r"cpu_set_kernel_stack\(",
            "scheduler should set TSS rsp0 before running process",
        )
        self._require(
            self.ctxsw_src,
            r"testb \$0x3,\s*%sil[\s\S]*iretq",
            "context switch should branch to iretq path for ring3 cs",
        )

    def test_ring3_smoke_process_wiring_exists(self):
        self._require(
            self.kernel_c_src,
            r"spawn_ring3_smoke_process",
            "kernel.c should define a ring3 smoke-process setup helper",
        )
        self._require(
            self.kernel_c_src,
            r"region->flags \|= MEM_REGION_FLAG_EXEC",
            "ring3 smoke setup should mark user linear region executable",
        )
        self._require(
            self.kernel_c_src,
            r"process_set_user_entry\(",
            "ring3 smoke setup should switch process context to user entry",
        )
        self._require(
            self.syscall_c_src,
            r"\(frame->cs & 0x3u\) != 0x3u",
            "syscall handler should detect CPL3-origin syscalls for ring3 smoke validation",
        )
        self._require(
            self.process_c_src,
            r"if \(\(frame->cs & 0x3u\) == 0x3u\)[\s\S]*return 0;",
            "IRQ preempt path should avoid rewriting return RIP for CPL3 frames",
        )
        self._require(
            self.syscall_c_src,
            r"syscall_finish_with_resched[\s\S]*process_should_resched",
            "syscall path should consume pending reschedule for CPL3 callers",
        )


if __name__ == "__main__":
    unittest.main()
