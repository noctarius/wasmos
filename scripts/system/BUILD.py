"""System image: initfs, the ESP tree, the disk fixtures, and the QEMU run and
test targets that boot them.

This manifest runs last: it consumes what every component published through
b.exports (kernel, loader, the packer, esp.files)."""

import json
import platform
import re
from pathlib import Path

from toolchains import arch

WFS_UUID = "5746532d74654573742d766f6c756d65"
WFS_VIRTIO_UUID = "5746532d76697274696f2d766f6c3031"
# Tool paths a ring-3 shadow tree inherits from its parent (configure options).
SHADOW_TOOLS = [
    "clang",
    "clangxx",
    "ld.lld",
    "lld-link",
    "llvm-objcopy",
    "llvm-nm",
    "llvm-ar",
    "wasm-ld",
    "zig",
    "asc",
    "rustc",
    "tinygo",
    "wasm-opt",
    "qemu",
    "ovmf-code",
    "ovmf-vars",
    "qemu-aarch64",
    "aavmf-code",
    "aavmf-vars",
]
# The SMP topology QEMU emulates for an SMP kernel.
SMP_CPUS = 4
# What run-qemu-bringup-test waits for on an architecture whose kernel cannot
# reach the CLI yet: the preemption self-test passed, which takes the MMU and
# demand paging, context switches, the scheduler, the timer interrupt and
# preemption of a running thread.
BRINGUP_MARKER = "[test] preempt ok"


def interactive_cpu_accel(machine, host_os):
    """CPU model and accelerators for the interactive run targets. An aarch64
    guest on an aarch64 host runs under the host's hypervisor: HVF on macOS
    (edk2 does not start with -cpu max there, so the host model), KVM on Linux
    with TCG as QEMU's fallback when /dev/kvm is absent. Everything else is
    TCG, which is what x86_64 is validated under. The test harness makes the
    same choice per session (scripts/qemu_test_framework.py default_accel)."""
    host = {"arm64": "aarch64", "amd64": "x86_64"}.get(
        platform.machine().lower(), platform.machine().lower()
    )
    if machine.name == "aarch64" and host == "aarch64":
        if host_os == "Darwin":
            return ["-accel", "hvf", "-cpu", "host"]
        return ["-accel", "kvm", "-accel", "tcg", "-cpu", "max"]
    return ["-cpu", "max"]


def initfs(b):
    """initfs.img + initfs_config.bin from the architecture's manifest
    (scripts/initfs.<arch>.toml) and bootstrap rules
    (scripts/initfs/devmgr/rules/default.<arch>.rules). Each architecture keeps
    its own complete set: the boot disk's driver and the platform drivers
    differ. Payloads are resolved by file name from the build directory, so
    each one named in the manifest is an input."""
    root = b.root
    name = arch(b.cfg).name
    toml = root / f"scripts/initfs.{name}.toml"
    b.regen_on(toml)
    payloads = re.findall(r'^\s*source\s*=\s*"([^"]+)"', toml.read_text(), re.M)
    rules = "devmgr_rules_init_default.rules"
    default_rules = root / f"scripts/initfs/devmgr/rules/default.{name}.rules"
    b.command(
        [rules],
        ["cp", default_rules, rules],
        inputs=[default_rules],
        description=f"COPY {rules}",
    )
    make = root / "scripts/make_initfs.py"
    b.command(
        ["initfs.img"],
        [
            b.tools["python"],
            make,
            "--manifest",
            toml,
            "--build-dir",
            ".",
            "--output",
            "initfs.img",
            "--config-output",
            "initfs_config.bin",
        ],
        inputs=[toml],
        implicit=[make, *dict.fromkeys(payloads)],
        implicit_outputs=["initfs_config.bin"],
        description="INITFS initfs.img",
    )
    return "initfs.img"


def disk_images(b):
    root = b.root
    mkfs = b.exports["mkfs_wfs"]
    content = b.glob("**/*", root=root / "scripts/wfs")
    for image, uuid in (("wfs.img", WFS_UUID), ("wfs_virtio.img", WFS_VIRTIO_UUID)):
        b.command(
            [image],
            [
                f"./{mkfs}",
                "--quiet",
                "--uuid",
                uuid,
                "--populate",
                root / "scripts/wfs",
                image,
                "16M",
            ],
            implicit=[mkfs, *content],
            description=f"MKFS {image}",
        )
    gpt = root / "scripts/make_gpt_image.py"
    userfs = root / "userfs"
    userfs.mkdir(exist_ok=True)
    b.command(
        ["user.img"],
        [
            b.tools["python"],
            gpt,
            "--output",
            "user.img",
            "--label",
            "user",
            "--source",
            userfs,
        ],
        implicit=[gpt, *b.glob("**/*", root=userfs)],
        description="GPT user.img",
    )
    # Interactive sessions boot copies, never the fixtures the tests expect.
    for image in ("wfs.img", "wfs_virtio.img", "user.img"):
        b.command(
            [f"run/{image}"],
            ["cp", image, f"run/{image}"],
            inputs=[image],
            description=f"COPY run/{image}",
        )
    return ["wfs.img", "wfs_virtio.img", "user.img"]


def esp(b, initfs_img):
    root, system = b.root, b.dir
    files = [
        (f"EFI/BOOT/{arch(b.cfg).efi_boot_name}", b.exports["bootloader"]),
        ("kernel.elf", b.exports["kernel.elf"]),
        ("kernel.cmdline", system / "kernel.cmdline"),
        ("initfs.img", initfs_img),
        ("system/sysinit.rc", system / "sysinit.rc"),
        # The boot rules, per architecture like the initfs set; installed under
        # the one name the device manager reads.
        (
            "system/devmgr/rules/default.rules",
            system / f"devmgr/rules/default.{arch(b.cfg).name}.rules",
        ),
        ("system/net/interfaces", system / "net/interfaces"),
        ("system/fonts/roboto.ttf", system / "fonts/Roboto-Regular.ttf"),
        ("system/fonts/roboto_mono.ttf", system / "fonts/RobotoMono-Regular.ttf"),
        ("system/fonts/roboto_serif.ttf", system / "fonts/RobotoSerif-Regular.ttf"),
        ("startup.nsh", root / "scripts/startup.nsh"),
        ("large_read.txt", root / "scripts/large_read.txt"),
        ("write_smoke.txt", root / "scripts/write_smoke.txt"),
        *b.exports.get("esp.files", []),
    ]
    trees = [
        ("system/keymaps", system / "keymaps"),
    ]
    # The TLS trust store is fetched on demand (scripts/fetch-ca-certs.sh); an
    # empty placeholder keeps the build offline, and net-stack then reports no
    # CA store.
    optional = [
        (
            "system/net/certificates/ca-certs.pem",
            system / "net/certificates/ca-certs.pem",
        )
    ]
    dirs = [
        "apps",
        "system/services",
        "system/drivers",
        "system/devices",
        "system/utils",
    ]

    def path(p):
        return str(p) if str(p).startswith("/") else str(b.out / p)

    plan = b.write_file(
        "esp-plan.json",
        json.dumps(
            {
                "files": [[d, path(s)] for d, s in files],
                "trees": [[d, str(s)] for d, s in trees],
                "optional": [[d, str(s)] for d, s in optional],
                # Presence of each optional source: removing one changes the
                # plan, which re-stages the placeholder.
                "optional_present": [Path(s).is_file() for _, s in optional],
                "dirs": dirs,
            },
            indent=1,
        )
        + "\n",
    )
    tree_inputs = [f for _, t in trees for f in b.glob("**/*", root=t)]
    # The trust store may appear, change or vanish after a build; globbing its
    # directory regenerates on creation/removal and tracks existing files.
    tree_inputs += [
        f for _, src in optional for f in b.glob("*", root=Path(src).parent)
    ]
    stage = root / "scripts/stage_esp.py"
    b.command(
        ["esp.stamp"],
        [
            b.tools["python"],
            stage,
            "--plan",
            plan,
            "--esp",
            "esp",
            "--stamp",
            "esp.stamp",
        ],
        inputs=[plan],
        implicit=[stage, *(s for _, s in files), *tree_inputs],
        description="ESP esp/",
    )
    return "esp.stamp"


def run_targets(b, esp_stamp, images):
    tools, out, root = b.tools, b.out, b.root
    machine = arch(b.cfg)
    smp = SMP_CPUS if b.cfg.WASMOS_SMP else 1
    deps = [esp_stamp, *images]
    ovmf_code = tools.get(machine.firmware_code_tool, "")
    ovmf_vars = tools.get(machine.firmware_vars_tool, "")
    qemu_bin = tools.get(machine.qemu_tool) or f"qemu-system-{machine.name}"
    vars_arg = ["--ovmf-vars", ovmf_vars] if ovmf_vars else []
    # The harness builds its QEMU command line for WASMOS_ARCH
    # (scripts/qemu_test_framework.py), from the same ARCHES entry.
    env = {
        "WASMOS_ARCH": machine.name,
        "WASMOS_QEMU": qemu_bin,
        "WASMOS_OVMF_CODE": ovmf_code,
        "WASMOS_OVMF_VARS": ovmf_vars,
        "WASMOS_ESP": out / "esp",
        "WASMOS_KERNEL": out / "kernel.elf",
        "WASMOS_USERFS_IMAGE": out / "user.img",
        "WASMOS_WFS_IMAGE": out / "wfs.img",
        "WASMOS_WFS_VIRTIO_IMAGE": out / "wfs_virtio.img",
        "WASMOS_QEMU_SMP_COUNT": smp,
        "PYTHONDONTWRITEBYTECODE": 1,
    }
    py = tools["python"]
    # The halt test's wall-clock budget: 120 s suits KVM/HVF; slow TCG hosts
    # (CI runners) raise it with WASMOS_QEMU_TEST_TIMEOUT at run time.
    b.run_target(
        "run-qemu-test",
        [
            "sh",
            "-c",
            'exec "$0" "$@" --timeout "${WASMOS_QEMU_TEST_TIMEOUT:-120}"',
            py,
            root / "scripts/qemu_halt_test.py",
            "--ovmf-code",
            ovmf_code,
            *vars_arg,
            "--esp",
            out / "esp",
            "--smp",
            smp,
        ],
        deps=deps,
        env=env,
        description="QEMU halt test",
    )
    # Boots to BRINGUP_MARKER and stops: the gate for an architecture under
    # bring-up, whose kernel cannot run run-qemu-test yet. Valid on any tree.
    b.run_target(
        "run-qemu-bringup-test",
        [
            py,
            root / "scripts/qemu_bringup_test.py",
            "--ovmf-code",
            ovmf_code,
            *vars_arg,
            "--esp",
            out / "esp",
            "--marker",
            BRINGUP_MARKER,
        ],
        deps=deps,
        env=env,
        description="QEMU bring-up marker test",
    )
    b.run_target(
        "run-qemu-cli-test",
        [py, root / "scripts/run_unittest_suite.py", "--start-dir", "tests"],
        deps=deps,
        env={**env, "WASMOS_QEMU_ISOLATE_ESP": 1},
        description="QEMU CLI integration tests",
    )
    b.run_target(
        "run-qemu-ui-test",
        [
            py,
            root / "scripts/qemu_ui_test.py",
            "--ovmf-code",
            ovmf_code,
            *vars_arg,
            "--esp",
            out / "esp",
            "--smp",
            smp,
        ],
        deps=deps,
        env=env,
        description="QEMU UI test",
    )

    run_images = [f"run/{i}" for i in ("user.img", "wfs.img", "wfs_virtio.img")]
    qemu = [
        qemu_bin,
        *machine.qemu_machine,
        "-m",
        "512M",
        *interactive_cpu_accel(machine, tools.get("host-os", "")),
        *(["-smp", str(smp)] if smp > 1 else []),
        "-nographic",
        "-serial",
        "mon:stdio",
        # The kernel-log port: a virtio console the virtio_serial driver feeds
        # from the kmsg ring, captured to syslog.log. Slot 0x1e keeps the other
        # devices' slots, from which the virtio-blk disk identity is derived.
        # Booting with log.serial=com2 moves the kernel log to COM2 instead
        # (add `-serial file:...` to capture it).
        "-device",
        "virtio-serial-pci,id=klogbus,addr=0x1e",
        "-chardev",
        f"file,id=klog,path={out / 'syslog.log'}",
        "-device",
        "virtconsole,bus=klogbus.0,chardev=klog",
        "-netdev",
        "user,id=net0",
        "-device",
        "virtio-net-pci,netdev=net0"
        + ("" if machine.qemu_nic_option_rom else ",romfile="),
        "-device",
        "virtio-rng-pci",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        *(["-drive", f"if=pflash,format=raw,file={ovmf_vars}"] if ovmf_vars else []),
        "-drive",
        f"format=raw,file=fat:rw:{out / 'esp'}",
        "-drive",
        f"format=raw,file={out / run_images[0]}",
        # The ata driver's WFS unit; a machine without IDE has no such unit.
        *(
            [
                "-drive",
                f"if=ide,index=2,media=disk,format=raw,file={out / run_images[1]}",
            ]
            if machine.qemu_ide
            else []
        ),
        "-drive",
        f"if=none,format=raw,file={out / run_images[2]},id=vwfs0",
        "-device",
        "virtio-blk-pci,drive=vwfs0,id=vwfs,addr=0x6.0",
    ]
    b.run_target("run-qemu", qemu, deps=[esp_stamp, *run_images], description="QEMU")
    display = [a for a in qemu if a != "-nographic"]
    b.run_target(
        "run-qemu-ui",
        display,
        deps=[esp_stamp, *run_images],
        description="QEMU (display)",
    )

    # Marker tests a ring-3 test configuration answers (WASMOS_RING3_SMOKE,
    # WASMOS_PM_TEST_HOOKS); run them in such a tree, or through the
    # run-qemu-ring3-*-test wrappers below, which build one.
    ring3 = [
        py,
        root / "scripts/qemu_ring3_halt_test.py",
        "--ovmf-code",
        ovmf_code,
        *vars_arg,
        "--esp",
        out / "esp",
    ]
    b.run_target(
        "run-qemu-ring3-check",
        ring3,
        deps=deps,
        env=env,
        description="QEMU ring3 smoke markers",
    )
    b.run_target(
        "run-qemu-ring3-threading-check",
        ring3 + ["--require-threading"],
        deps=deps,
        env=env,
        description="QEMU ring3 threading markers",
    )
    b.run_target(
        "run-qemu-ring3-fault-storm-check",
        [
            py,
            root / "scripts/qemu_ring3_fault_storm_test.py",
            "--ovmf-code",
            ovmf_code,
            *vars_arg,
            "--esp",
            out / "esp",
        ],
        deps=deps,
        env=env,
        description="QEMU ring3 fault-storm markers",
    )
    # Needs a kernel configured with WASMOS_SCHED_SMP_STRESS; without it the
    # script refuses up front rather than timing out.
    b.run_target(
        "run-qemu-sched-stress-test",
        [
            py,
            root / "scripts/qemu_sched_stress_test.py",
            "--ovmf-code",
            ovmf_code,
            *vars_arg,
            "--esp",
            out / "esp",
            "--smp",
            smp,
        ],
        deps=deps,
        env=env,
        description="QEMU SMP scheduler stress",
    )

    # Ring-3 gates build their own wasm3 tree with the test probes enabled,
    # beside this one, and run its marker check there.
    configure = [py, root / "configure"]
    trace = "y" if b.cfg.WASMOS_TRACE else "n"
    shadows = [
        ("run-qemu-ring3-test", "ring3", "run-qemu-ring3-check", []),
        (
            "run-qemu-ring3-threading-test",
            "ring3-threading",
            "run-qemu-ring3-threading-check",
            ["WASMOS_RING3_THREAD_LIFECYCLE_SMOKE=y"],
        ),
        (
            "run-qemu-ring3-fault-storm-test",
            "ring3-fault-storm",
            "run-qemu-ring3-fault-storm-check",
            [],
        ),
    ]
    # The shadow tree builds and boots with exactly this tree's toolchain and
    # firmware, including paths given to this tree's configure.
    inherited = [f"--{key}={tools[key]}" for key in SHADOW_TOOLS if tools.get(key)]
    for target, subdir, check, extra in shadows:
        tree = out / subdir
        sets = [
            "WASMOS_WASM_RUNTIME_WASM3=y",
            "WASMOS_RING3_SMOKE=y",
            "WASMOS_PM_TEST_HOOKS=y",
            f"WASMOS_ARCH_{machine.name.upper()}=y",
            f"WASMOS_TRACE={trace}",
            *extra,
        ]
        b.run_target(
            target,
            [
                configure
                + [tree, root / "configs/wasmos_defconfig", "--reseed", "--no-ide-link"]
                + [x for a in sets for x in ("--set", a)]
                + inherited,
                ["ninja", "-C", tree, check],
            ],
            description=f"{subdir} tree: {check}",
        )
    b.run_target(
        "strict-ring3",
        [
            ["ninja", "-C", out, "run-qemu-test"],
            ["ninja", "-C", out, "run-qemu-ring3-test"],
        ],
        description="strict ring3 gate (run-qemu-test + run-qemu-ring3-test)",
    )
    gdb_port = b.cfg.get("QEMU_GDB_PORT", 1234)
    b.run_target(
        "run-qemu-debug",
        qemu[:1] + ["-S", "-gdb", f"tcp::{gdb_port}"] + qemu[1:],
        deps=[esp_stamp, *run_images],
        description=f"QEMU (gdb :{gdb_port})",
    )


def build(b):
    stamp = esp(b, initfs(b))
    images = disk_images(b)
    b.alias("esp", stamp, default=True)
    b.alias("images", *images, default=True)
    run_targets(b, stamp, images)
