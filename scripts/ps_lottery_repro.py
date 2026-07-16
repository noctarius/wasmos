#!/usr/bin/env python3
# Local repro: boot once, send `ps` many times, classify each outcome.
import os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from qemu_test_framework import QemuSession, default_config

N = int(os.environ.get("PS_N", "30"))
cfg = default_config()
s = QemuSession(cfg, timeout_s=120, echo=False)
s.start()
if not s.expect(b"wamos> "):
    print("NO-PROMPT")
    sys.exit(2)

ok = silent = fault = 0
for i in range(N):
    m = s.mark()
    s.send("ps")
    got = s.expect_from(m, b"vm(bytes)", timeout_s=8)
    s.expect_from(m, b"wamos> ", timeout_s=8)
    tail = s.tail()
    if isinstance(tail, bytes):
        tail = tail.decode("utf-8", "replace")
    if "user-pf" in tail or "terminate pid" in tail:
        fault += 1
        print(f"{i}: FAULT\n--- tail ---\n{tail}\n------------")
    elif got:
        ok += 1
        print(f"{i}: OK")
    else:
        silent += 1
        print(f"{i}: SILENT\n--- tail ---\n{tail}\n------------")
    sys.stdout.flush()
print(f"RESULT ok={ok} silent={silent} fault={fault} of {N}")
try:
    s.send("halt")
    s.close()
except Exception:
    pass
