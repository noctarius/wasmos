#!/usr/bin/env python3
"""Run the integration suite, whole or one battery at a time.

Batteries are declared in tests/batteries.json and partition the suite by
subsystem, one per CI runner. This script is one of that manifest's two
consumers; .github/workflows/ci.yml builds its matrix from the same file.

Modes:
    (no args)              run everything discovered under --start-dir
    --battery NAME         run only that battery's files
    --list-batteries       print the battery names, one per line
    --matrix               print the CI matrix as JSON: one cell per QEMU
                           battery PER (ARCH, RUNTIME), one per host battery
    --bringup-matrix       print the bring-up configurations as JSON: one cell
                           per (arch, defconfig) CI builds and boots to the
                           bring-up marker, with no battery
    --host-matrix          print the CI build/test hosts as JSON: one cell per
                           host that has a CI runner (hosts without one are
                           tested locally only)
    --verify-batteries     fail unless every discovered file is in exactly one
                           battery, and every listed file exists

--verify-batteries is the guard that makes named batteries safe. A numeric
i-of-N shard covers every file by construction; a named battery does not, so a
newly added test could belong to nothing and never run with no one the wiser.
It boots nothing, so the quality gate can run it.

WASMOS_TEST_BATTERY selects a battery when --battery is not passed, so the
existing ninja targets can be driven per battery from the environment without
threading a new argument through them.
"""

import argparse
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO_ROOT, "tests", "batteries.json")


def load_manifest():
    with open(MANIFEST, "r", encoding="utf-8") as f:
        return json.load(f)


def load_batteries():
    return load_manifest()["batteries"]


def load_arches():
    """The target architectures, each with the wasm runtimes every QEMU battery
    runs under on it and the bring-up configurations CI builds and boots to
    the bring-up marker.

    A manifest that predates the architecture axis lists its runtimes at the
    top level; they are x86_64's. One that predates the runtime axis too means
    "just the default build".
    """
    manifest = load_manifest()
    if "arches" in manifest:
        return manifest["arches"]
    return [{"name": "x86_64", "runtimes": manifest.get("runtimes", []), "bringup": []}]


def load_hosts():
    """The machines builds and host tests run on, each with its CI runner. A
    manifest without the axis means the one x86_64 Linux runner."""
    return load_manifest().get(
        "hosts", [{"name": "x86_64-linux", "runner": "ubuntu-24.04"}]
    )


def discovered_files(start_dir):
    d = start_dir if os.path.isabs(start_dir) else os.path.join(REPO_ROOT, start_dir)
    return sorted(
        f for f in os.listdir(d) if f.startswith("test_") and f.endswith(".py")
    )


def verify(start_dir):
    """Every discovered file in exactly one battery, every listed file present."""
    batteries = load_batteries()
    listed = {}
    problems = []
    for b in batteries:
        for f in b["files"]:
            if f in listed:
                problems.append(f"{f} is in both '{listed[f]}' and '{b['name']}'")
            listed[f] = b["name"]
    found = set(discovered_files(start_dir))
    for f in sorted(found - set(listed)):
        problems.append(f"{f} exists but is in no battery — it would never run")
    for f in sorted(set(listed) - found):
        problems.append(f"{f} is in battery '{listed[f]}' but does not exist")
    if problems:
        for p in problems:
            print(f"batteries: {p}", file=sys.stderr)
        print(
            "batteries: fix tests/batteries.json — every test file belongs to "
            "exactly one battery",
            file=sys.stderr,
        )
        return 1
    print(
        f"batteries: {len(found)} test files across {len(batteries)} batteries, "
        "each in exactly one"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run unittest suite with deterministic status markers."
    )
    parser.add_argument("--start-dir", default="tests")
    parser.add_argument("--battery", default=os.environ.get("WASMOS_TEST_BATTERY", ""))
    parser.add_argument("--list-batteries", action="store_true")
    parser.add_argument("--matrix", action="store_true")
    parser.add_argument("--bringup-matrix", action="store_true")
    parser.add_argument("--host-matrix", action="store_true")
    parser.add_argument("--verify-batteries", action="store_true")
    args = parser.parse_args()

    if args.list_batteries:
        for b in load_batteries():
            print(b["name"])
        return 0
    if args.matrix:
        # A QEMU battery is one cell PER (ARCH, RUNTIME); a host battery boots
        # nothing, so it has neither and appears once. Emitting the cross
        # product here rather than in the workflow keeps tests/batteries.json
        # the only place any axis is written down.
        pairs = [(a["name"], r) for a in load_arches() for r in a.get("runtimes", [])]
        cells = []
        for b in load_batteries():
            if b["needs_qemu"] and pairs:
                cells.extend(
                    {"battery": b["name"], "needs_qemu": True, "arch": a, "runtime": r}
                    for a, r in pairs
                )
            else:
                cells.append({"battery": b["name"], "needs_qemu": b["needs_qemu"]})
        print(json.dumps(cells))
        return 0
    if args.bringup_matrix:
        print(
            json.dumps(
                [
                    {"arch": a["name"], "config": c}
                    for a in load_arches()
                    for c in a.get("bringup", [])
                ]
            )
        )
        return 0
    if args.host_matrix:
        print(json.dumps([h for h in load_hosts() if h.get("runner")]))
        return 0
    if args.verify_batteries:
        return verify(args.start_dir)

    cmd = [sys.executable, "-m", "unittest"]
    label = "cli suite"
    if args.battery:
        names = [b["name"] for b in load_batteries()]
        battery = next((b for b in load_batteries() if b["name"] == args.battery), None)
        if battery is None:
            print(
                f"[test] unknown battery '{args.battery}' (have: {', '.join(names)})",
                file=sys.stderr,
            )
            return 2
        label = f"battery {battery['name']}"
        # Module paths, so unittest imports them the same way discovery does.
        cmd += [f"{args.start_dir}.{f[:-3]}" for f in battery["files"]]
    else:
        cmd += ["discover", "-s", args.start_dir]

    rc = subprocess.call(cmd)
    if rc == 0:
        print(f"[test] {label} status ok")
        return 0
    print(f"[test] {label} status failed")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
