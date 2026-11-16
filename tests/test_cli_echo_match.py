#!/usr/bin/env python3
"""`find_echo` must find the CLI's echo of a command, and only that echo.

Service logs and the CLI share COM1, and the CLI echoes a command one keystroke
at a time while klog writes whole lines. A log line that arrives while the echo
is being printed lands inside it, so the serial stream holds the command split
around a complete foreign line. A helper that waits for the echo as one
unbroken byte string then times out against a healthy, idle guest.

These run on the host on plain bytes: the split depends on when a service
happens to log, which a guest cannot be made to do on demand.

Regression: 2026-11-15-split-echo -- a service log line spliced into the echo
("/ wamos> fs_leave_open /[net-stack] tls: no CA trust store\\nleakfds/held 8")
made VfsClientReleaseTest fail with "'fs_leave_open /leakfds/held 8' was never
echoed" although the command had run and printed its result (CI run
43608707742, wasm3_smp, filesystem battery). Cost: a red battery and a rerun
for every late log line that happens to coincide with a typed command.
"""

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from scripts.qemu_test_framework import QemuSession, find_echo

CMD = "fs_leave_open /leakfds/held 8"
PROMPT = b"/ wamos> "
LOG = b"[net-stack] tls: no CA trust store"


class SplitEchoTest(unittest.TestCase):
    def test_echo_split_by_a_service_log_line_is_found(self):
        """The bytes CI captured: the echo, a log line, the rest of the echo."""
        buf = (
            PROMPT
            + b"fs_leave_open /"
            + LOG
            + b"\nleakfds/held 8\nfs-leave-open: left open\n"
            + PROMPT
        )
        found = find_echo(buf, 0, CMD)
        self.assertIsNotNone(found, "the split echo was not recognised")
        begin, end = found
        self.assertEqual(begin, len(PROMPT))
        self.assertEqual(buf[end:], b"\nfs-leave-open: left open\n" + PROMPT)

    def test_several_injected_lines_with_crlf_endings_are_stepped_over(self):
        buf = (
            PROMPT
            + b"fs_leave"
            + b"[test] stage one ok\r\n"
            + b"[diag] ipc: 3 waiters\r\n"
            + b"_open /leakfds/held "
            + LOG
            + b"\r\n8\r\nfs-leave-open: left open\r\n"
        )
        found = find_echo(buf, 0, CMD)
        self.assertIsNotNone(found, "an echo split three times was not recognised")
        self.assertEqual(buf[found[1] :], b"\r\nfs-leave-open: left open\r\n")

    def test_contiguous_echo_reports_the_same_span_as_a_plain_search(self):
        buf = PROMPT + CMD.encode() + b"\nfs-leave-open: left open\n" + PROMPT
        begin = buf.index(CMD.encode())
        self.assertEqual(find_echo(buf, 0, CMD), (begin, begin + len(CMD)))

    def test_an_echo_before_start_is_not_reported(self):
        first = PROMPT + CMD.encode() + b"\n"
        buf = first + PROMPT
        self.assertIsNone(find_echo(buf, len(first), CMD))

    def test_a_log_line_around_the_echo_is_not_part_of_it(self):
        """A line printed before the first or after the last echoed byte belongs
        to the stream, not the echo: the span covers the command alone."""
        buf = PROMPT + LOG + b"\n" + CMD.encode() + LOG + b"\n"
        begin = buf.index(CMD.encode())
        self.assertEqual(find_echo(buf, 0, CMD), (begin, begin + len(CMD)))


class NoOverMatchTest(unittest.TestCase):
    """Only whole bracket-tagged lines may sit inside an echo. Anything looser
    lets a wrong command or a garbled echo pass as the one that was typed."""

    def test_a_different_command_is_not_matched(self):
        buf = PROMPT + b"fs_leave_open /" + LOG + b"\nleakfds/held 9\n"
        self.assertIsNone(find_echo(buf, 0, CMD))

    def test_a_partial_injected_line_is_rejected(self):
        """No newline: the bytes are not a whole log line, so the echo is garbled."""
        buf = PROMPT + b"fs_leave_open /" + LOG + b"leakfds/held 8\n"
        self.assertIsNone(find_echo(buf, 0, CMD))

    def test_an_untagged_injected_line_is_rejected(self):
        buf = PROMPT + b"fs_leave_open /stray output\nleakfds/held 8\n"
        self.assertIsNone(find_echo(buf, 0, CMD))

    def test_a_dropped_keystroke_is_rejected(self):
        buf = PROMPT + b"fs_leave_open /" + LOG + b"\neakfds/held 8\n"
        self.assertIsNone(find_echo(buf, 0, CMD))

    def test_a_bare_newline_inside_the_echo_is_rejected(self):
        buf = PROMPT + b"fs_leave_open /\nleakfds/held 8\n"
        self.assertIsNone(find_echo(buf, 0, CMD))


class _ScriptedSession(QemuSession):
    """A QemuSession whose guest is a script: each pump appends the next chunk.

    send_command is the method under test, so only the transport is replaced --
    no QEMU, no serial port -- and the chunks arrive in the order a real guest
    produces them, which is what decides where the echo is taken to end.
    """

    def __init__(self, before: bytes, chunks):
        self.buf = bytearray(before)
        self.timeout_s = 2
        self._chunks = list(chunks)

    def send(self, line: str) -> None:
        pass

    def _pump(self, timeout_s: float) -> None:
        if self._chunks:
            self.buf += self._chunks.pop(0)

    def dump_stall_state(self, reason: str) -> str:
        return ""


BODY = b"wasmos-verify-body-42"
SPAWN = "spawn /boot/system/utils/curl https://10.0.2.2:5583/hello"


class EchoEndTest(unittest.TestCase):
    """send_command's offset is where the command's OUTPUT starts.

    Regression: 2026-11-16-echo-end-before-newline -- the offset was the end of
    the echoed characters, but the CLI writes the line's newline after them. A
    log line arriving in between makes the vt erase the open line and repaint
    it after the log line (src/services/vt/vt_outmux.c), and the repaint carries
    whatever else was on that line: the previous command's output, the prompt.
    Both then sat after the offset, so a negative assertion on the command's
    output saw the previous body (a false "verification bypassed!" in
    test_net_stack_https_verify_e2e) and a prompt search matched the repainted
    prompt before the command ran (test_vt_tty_switch).
    """

    def _end_of(self, chunks):
        before = b"\r\n/ wamos> " + BODY
        session = _ScriptedSession(before, chunks)
        end = session.send_command(SPAWN, timeout_s=2)
        while session._chunks:
            session._pump(0)
        return bytes(session.buf[end:])

    def test_a_log_line_before_the_newline_does_not_leak_the_repaint(self):
        after = self._end_of(
            [
                SPAWN.encode(),
                b"\r\x1b[K[calculator] ready\r\n/ wamos> " + BODY + SPAWN.encode(),
                b"\r\n",
                b"[curl] connect failed\r\n/ wamos> ",
            ]
        )
        self.assertNotIn(
            BODY, after, "the repainted line is counted as the command's output"
        )
        self.assertTrue(after.startswith(b"[curl] connect failed"), after)

    def test_a_wrapped_repaint_is_stepped_over(self):
        after = self._end_of(
            [
                SPAWN.encode(),
                b"\r\x1b[1A\x1b[J[calculator] ready\r\n/ wamos> "
                + BODY
                + SPAWN.encode(),
                b"\r\n[curl] connect failed\r\n",
            ]
        )
        self.assertNotIn(BODY, after)
        self.assertTrue(after.startswith(b"[curl] connect failed"), after)

    def test_a_plain_echo_ends_after_its_newline(self):
        after = self._end_of([SPAWN.encode() + b"\r\n[curl] connect failed\r\n"])
        self.assertTrue(after.startswith(b"[curl] connect failed"), after)

    def test_a_log_line_inside_the_echo_still_ends_after_the_newline(self):
        after = self._end_of(
            [
                SPAWN[:5].encode(),
                b"\r\x1b[K[calculator] ready\r\n/ wamos> " + BODY + SPAWN[:5].encode(),
                SPAWN[5:].encode() + b"\r\n[curl] connect failed\r\n",
            ]
        )
        self.assertNotIn(BODY, after)
        self.assertTrue(after.startswith(b"[curl] connect failed"), after)


if __name__ == "__main__":
    unittest.main()
