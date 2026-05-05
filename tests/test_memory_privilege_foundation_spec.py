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
        self._require(
            self.paging_src,
            r"paging_verify_user_root_impl[\s\S]*unexpected pml4",
            "paging.c should verify user roots do not expose unexpected kernel PML4 slots",
        )
        self._require(
            self.paging_src,
            r"paging_create_address_space[\s\S]*paging_verify_user_root_impl\(root,\s*1\)",
            "child address-space creation should verify kernel mapping footprint",
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
        self.assertNotRegex(
            self.wasm3_link_src,
            re.compile(r"require_io_capability[\s\S]*capability_context_configured"),
            msg="io capability helper should not keep compatibility-mode implicit allow bypass",
        )
        self._require(
            self.wasm3_link_src,
            r"require_mmio_capability",
            "wasm3_link.c should define mmio capability requirement helper",
        )
        self._require(
            self.wasm3_link_src,
            r"wasmos_framebuffer_map[\s\S]*require_mmio_capability",
            "wasmos_framebuffer_map should enforce mmio capability",
        )
        self._require(
            self.wasm3_link_src,
            r"require_dma_capability",
            "wasm3_link.c should define dma capability requirement helper",
        )
        self._require(
            self.wasm3_link_src,
            r"wasmos_shmem_create[\s\S]*require_dma_capability",
            "wasmos_shmem_create should enforce dma capability",
        )
        self._require(
            self.wasm3_link_src,
            r"wasmos_shmem_map[\s\S]*require_dma_capability",
            "wasmos_shmem_map should enforce dma capability",
        )

    def test_framebuffer_info_uses_validated_user_va_for_copy(self):
        self._require(
            self.wasm3_link_src,
            r"wasmos_framebuffer_info[\s\S]*mm_copy_to_user\(\s*proc->context_id,\s*out_user,",
            "wasmos_framebuffer_info should copy via the validated user VA, not raw host pointer",
        )
        self._require(
            self.wasm3_link_src,
            r"wasmos_boot_config_copy[\s\S]*(mm_copy_to_user\(\s*proc->context_id,\s*ptr_user,|wasm_copy_to_user_sync_views\(\s*proc->context_id,\s*ptr_user,)",
            "wasmos_boot_config_copy should copy via validated user VA (direct copy or coherence helper)",
        )

    def test_user_copy_helpers_use_bounce_buffer_across_cr3_switch(self):
        with open(os.path.join(os.path.dirname(self.paging_path), "memory.c"), "r", encoding="utf-8") as f:
            memory_src = f.read()
        self._require(
            memory_src,
            r"mm_copy_to_user(_impl)?[\s\S]*uint8_t bounce\[256\][\s\S]*memcpy\(bounce,\s*src_bytes,\s*\(size_t\)n\)[\s\S]*paging_switch_root\((ctx->root_table|args->root_table)\)[\s\S]*memcpy\(\(void \*\)\(uintptr_t\)user_cur,\s*bounce,\s*\(size_t\)n\)",
            "mm_copy_to_user path should stage kernel bytes in bounce buffer before user-root write",
        )
        self._require(
            memory_src,
            r"mm_copy_from_user(_impl)?[\s\S]*uint8_t bounce\[256\][\s\S]*paging_switch_root\((ctx->root_table|args->root_table)\)[\s\S]*memcpy\(bounce,\s*\(const void \*\)\(uintptr_t\)user_cur,\s*\(size_t\)n\)[\s\S]*paging_switch_root\((prev_root|args->prev_root)\)[\s\S]*memcpy\(dst_bytes,\s*bounce,\s*\(size_t\)n\)",
            "mm_copy_from_user path should stage user bytes in bounce buffer before kernel-root write",
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
            r"if \(\(frame->cs & 0x3u\) == 0x3u\)[\s\S]*frame->cs = KERNEL_CS_SELECTOR;",
            "IRQ preempt path should rewrite CPL3 frames to kernel CS before trampoline return",
        )
        self._require(
            self.process_c_src,
            r"frame->rip = \(uint64_t\)\(uintptr_t\)process_preempt_trampoline;",
            "IRQ preempt path should redirect return RIP to preempt trampoline",
        )
        self._require(
            self.isr_src,
            r"cmpq \$KERNEL_CS_SELECTOR,\s*176\(%rsp\)",
            "IRQ0 integrity check should allow intentional CS rewrite to kernel selector",
        )

    def test_irq_route_surface_remains_kernel_only_until_capability_api_lands(self):
        self.assertNotRegex(
            self.wasm3_link_src,
            re.compile(r"wasm3_link_raw\([^\n]*\"wasmos\",[^\n]*\"irq_"),
            msg="wasm3 hostcall table should not expose user IRQ routing API before CAP_IRQ_ROUTE gating is implemented",
        )


if __name__ == "__main__":
    unittest.main()
