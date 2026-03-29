import os
import re
import unittest


class VtMemoryAllocationSpecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        cls.vt_path = os.path.join(root, "src", "services", "vt", "vt_main.c")
        cls.vt_cmake = os.path.join(root, "src", "services", "vt", "CMakeLists.txt")
        with open(cls.vt_path, "r", encoding="utf-8") as f:
            cls.vt_src = f.read()
        with open(cls.vt_cmake, "r", encoding="utf-8") as f:
            cls.cmake_src = f.read()

    def _require_vt(self, pattern: str) -> None:
        self.assertRegex(
            self.vt_src,
            re.compile(pattern, re.DOTALL),
            msg=f"Missing VT allocation pattern: {pattern}",
        )

    def _require_cmake(self, pattern: str) -> None:
        self.assertRegex(
            self.cmake_src,
            re.compile(pattern, re.DOTALL),
            msg=f"Missing VT CMake memory pattern: {pattern}",
        )

    def test_dynamic_allocation_uses_memory_grow(self):
        self._require_vt(r"__heap_base")
        self._require_vt(r"__builtin_wasm_memory_grow\(0,\s*1\)")
        self._require_vt(r"vt_alloc_tty_cells\(")

    def test_fallback_is_monotonic_and_logs_failures(self):
        self._require_vt(r"vt_log_alloc_failure\(\"runtime-grid\"")
        self._require_vt(r"vt_log_alloc_failure\(\"default-grid\"")
        self._require_vt(
            r"vt_heap_init\(\);\s*if\s*\(vt_alloc_tty_cells\(\)\s*!=\s*0\)\s*\{"
            r"[\s\S]*?g_vt_cols\s*=\s*VT_COLS_DEFAULT;"
            r"[\s\S]*?g_vt_rows\s*=\s*VT_ROWS_DEFAULT;"
            r"[\s\S]*?vt_reset_tty_cells\(\);"
            r"[\s\S]*?if\s*\(vt_alloc_tty_cells\(\)\s*!=\s*0\)"
        )
        self.assertNotRegex(
            self.vt_src,
            re.compile(
                r"if\s*\(vt_alloc_tty_cells\(\)\s*!=\s*0\)\s*\{[\s\S]*?vt_heap_init\(\)",
                re.DOTALL,
            ),
            msg="Fallback path should not re-run vt_heap_init (no heap rewind).",
        )

    def test_vt_memory_limits_support_growth(self):
        self._require_cmake(r"--initial-memory=131072")
        self._require_cmake(r"--max-memory=1048576")


if __name__ == "__main__":
    unittest.main()
