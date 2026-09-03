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


class CliIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _cmd_expect(self, cmd: str, needle: bytes, timeout_s: int = 20) -> None:
        mark = self.session.mark()
        self.session.send(cmd)
        ok = self.session.expect_from(mark, needle, timeout_s=timeout_s)
        if not ok:
            self.fail(
                f"Expected output not found for '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )
        ok = self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
        if not ok:
            self.fail(
                f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )

    def test_help_lists_commands(self):
        self._cmd_expect("help", b"commands:")

    def test_date_reads_the_rtc(self):
        """End-to-end over the RTC driver: `date` looks the service up, sends
        RTC_IPC_READ_REQ and formats the reply. This is the only client of that
        driver, so it is what proves its request path, not just that it starts.
        Asserted on the shape (a 4-digit year and a HH:MM:SS), since the value
        is whatever the host clock says."""
        self._cmd_expect("date", re.compile(rb"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}"))

    def test_kmaps_dumps_page_tables(self):
        """Regression: 2026-08-31-warp-kmaps-stub. The WARP backend implemented
        kmap_dump/kmap_dump_all as no-op stubs returning 0, so `kmaps` printed
        "dumped" on the default (WARP) runtime while producing no dump at all --
        a diagnostic that reported success for work it never did. The page-table
        walk it performs is runtime-agnostic kernel state, so the stub cost the
        one runtime whose guests actually execute at CPL=3 against that root.
        Asserted on the dump reaching the log, not on the CLI's reply, since the
        reply was the part that was already (misleadingly) correct."""
        self._cmd_expect("kmaps", b"[paging] dump root=")

    def test_kmaps_all_dumps_every_context(self):
        """Companion to test_kmaps_dumps_page_tables for the all-contexts form,
        which was stubbed identically. The per-pid label is what makes the dump
        attributable, so it is what this asserts.

        Regression: 2026-08-31-wasm3-kmaps-trace-gated. Under wasm3 every label
        line sat inside trace_do(), which compiles out at the default
        WASMOS_TRACE=0, so the default build dumped page tables with nothing
        saying which process each belonged to -- and the label it would have
        printed with tracing on named its fields in a different order from
        WARP's, which no assertion on one shape can cover. Cost: the whole
        boot-and-init battery red on every wasm3_smp run."""
        self._cmd_expect("kmaps all", re.compile(rb"\[kmap\] pid=\d+ name="))

    def test_kmaps_all_reports_how_many_contexts_it_dumped(self):
        """Regression: 2026-08-31-kmaps-silent-empty-dump. Both backends skip
        every context they cannot resolve, silently, and used to return 0
        regardless -- so a dump that labelled NOTHING was indistinguishable from
        one that covered the system, and the CLI answered "dumped to kernel log"
        either way. That is the same defect class as the stub these tests were
        written for, one layer in. The count is what makes the difference
        observable, so a non-zero one is asserted rather than the bare marker."""
        self._cmd_expect(
            "kmaps all", re.compile(rb"\[kmap\] contexts end count=[1-9]\d*")
        )

    def test_ps_lists_processes(self):
        self._cmd_expect(
            "ps", b"vm(bytes) kstack(bytes) heap(bytes) rss_est(bytes) cpu(ticks) name"
        )
        self._cmd_expect("ps", b"cli")
        self._cmd_expect("ps", b"fs-manager")

    def test_ps_tree_lists_hierarchy(self):
        self._cmd_expect("ps tree", b"tree:")
        self._cmd_expect("ps tree", b"cli (pid")
        self._cmd_expect("ps tree", b"fs-manager (pid")

    def test_ps_all_lists_table_and_tree(self):
        self._cmd_expect(
            "ps all",
            b"vm(bytes) kstack(bytes) heap(bytes) rss_est(bytes) cpu(ticks) name",
        )
        self._cmd_expect("ps all", b"cli")
        self._cmd_expect("ps all", b"fs-manager")
        self._cmd_expect("ps all", b"tree:")
        self._cmd_expect("ps all", b"cli (pid")

    def test_ls_lists_root(self):
        self._cmd_expect("ls", b"boot")

    def test_cd_and_ls_apps(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd boot", b"/boot wamos>")
        self._cmd_expect("ls", b"startup.nsh")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_cd_nested_services(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd boot", b"/boot wamos>")
        self._cmd_expect("cd system", b"/boot/system wamos>")
        self._cmd_expect("cd services", b"/boot/system/services wamos>")
        self._cmd_expect("ls", b"cli.wap")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_cd_nested_drivers(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd boot", b"/boot wamos>")
        self._cmd_expect("cd system", b"/boot/system wamos>")
        self._cmd_expect("cd drivers", b"/boot/system/drivers wamos>")
        self._cmd_expect("ls", b"ata.wap")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_initfs_nested_relative_cd(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd init", b"/init wamos>")
        self._cmd_expect("cd devmgr", b"/init/devmgr wamos>")
        self._cmd_expect("ls", b"rules")
        self._cmd_expect("cd rules", b"/init/devmgr/rules wamos>")
        self._cmd_expect("ls", b"default.rules")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_initfs_cat_default_rules(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd init", b"/init wamos>")
        self._cmd_expect("cd devmgr", b"/init/devmgr wamos>")
        self._cmd_expect("cd rules", b"/init/devmgr/rules wamos>")
        self._cmd_expect("cat default.rules", b'RUN+="system/drivers/ata.wap"')
        self._cmd_expect("cd /", b"/ wamos>")

    def test_cd_dot_and_dotdot(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd boot", b"/boot wamos>")
        self._cmd_expect("ls", b"startup.nsh")
        self._cmd_expect("cd ..", b"/ wamos>")

    def test_cat_startup(self):
        """`cat` a boot-volume file by relative name, from inside /boot.

        The `cd` is not incidental. This case named no directory at all, so what
        it resolved against was whatever the previously-run test happened to
        leave behind -- and a relative name now means the client's working
        directory, exactly. Stating the directory makes the case independent of
        test order, as every other case in this file already is.
        """
        self._cmd_expect("cd /boot", b"/boot wamos>")
        self._cmd_expect("cat startup.nsh", b"BOOTX64.EFI")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_export_and_echo_variable(self):
        self._cmd_expect("export FOO=bar", b"wamos> ")
        self._cmd_expect("set FOO=bar", b"wamos> ")
        self._cmd_expect("echo ${FOO}", b"bar")
        self._cmd_expect("echo hello world", b"hello world")
        self._cmd_expect('echo "hi ${FOO}"', b"hi bar")
        self._cmd_expect("echo -- -n literal", b"-n literal")
        self._cmd_expect("set FOO=", b"wamos> ")
        self._cmd_expect("export FOO=", b"wamos> ")
        mark = self.session.mark()
        self.session.send("echo ${FOO}")
        self.assertTrue(
            self.session.expect_from(mark, b"\n", timeout_s=20), self.session.tail()
        )
        self.assertTrue(
            self.session.expect_from(mark, b"wamos> ", timeout_s=20),
            self.session.tail(),
        )

    def test_path_lookup_for_exec(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("export PATH=/boot/apps", b"wamos> ")
        self._cmd_expect("init_smoke", b"init-smoke: init start")
        self._cmd_expect("export PATH=", b"wamos> ")
        self._cmd_expect("init_smoke", b"no such command found: init_smoke")
        self._cmd_expect(
            "export PATH=/boot/apps:/boot/system/services:/boot/system/drivers:/boot/system/utils",
            b"wamos> ",
        )


if __name__ == "__main__":
    unittest.main()
