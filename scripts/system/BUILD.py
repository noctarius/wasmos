"""System image: initfs, the ESP tree, the disk fixtures, and the QEMU run and
test targets that boot them.

This manifest runs last: it consumes what every component published through
b.exports (kernel, loader, the packer, esp.files)."""

import json
import re

WFS_UUID = "5746532d74654573742d766f6c756d65"
WFS_VIRTIO_UUID = "5746532d76697274696f2d766f6c3031"
# The SMP topology QEMU emulates for an SMP kernel.
SMP_CPUS = 4


def initfs(b):
    """initfs.img + initfs_config.bin from scripts/initfs.toml. Payloads are
    resolved by file name from the build directory, so each one named in the
    manifest is an input."""
    root = b.root
    toml = root / "scripts/initfs.toml"
    b.regen_on(toml)
    payloads = re.findall(r'^\s*source\s*=\s*"([^"]+)"', toml.read_text(), re.M)
    rules = "devmgr_rules_init_default.rules"
    default_rules = root / "scripts/initfs/devmgr/rules/default.rules"
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
        ("EFI/BOOT/BOOTX64.EFI", b.exports["bootloader"]),
        ("kernel.elf", b.exports["kernel.elf"]),
        ("kernel.cmdline", system / "kernel.cmdline"),
        ("initfs.img", initfs_img),
        ("system/sysinit.rc", system / "sysinit.rc"),
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
        ("system/devmgr/rules", system / "devmgr/rules"),
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
                "dirs": dirs,
            },
            indent=1,
        )
        + "\n",
    )
    tree_inputs = [f for _, t in trees for f in b.glob("**/*", root=t)]
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
    smp = SMP_CPUS if b.cfg.WASMOS_SMP else 1
    deps = [esp_stamp, *images]
    ovmf_code, ovmf_vars = tools["ovmf-code"], tools["ovmf-vars"]
    vars_arg = ["--ovmf-vars", ovmf_vars] if ovmf_vars else []
    env = {
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
    b.run_target(
        "run-qemu-test",
        [
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
        tools["qemu"] or "qemu-system-x86_64",
        "-m",
        "512M",
        "-cpu",
        "max",
        *(["-smp", str(smp)] if smp > 1 else []),
        "-nographic",
        "-serial",
        "mon:stdio",
        "-serial",
        f"file:{out / 'syslog.log'}",
        "-netdev",
        "user,id=net0",
        "-device",
        "virtio-net-pci,netdev=net0",
        "-device",
        "virtio-rng-pci",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        *(["-drive", f"if=pflash,format=raw,file={ovmf_vars}"] if ovmf_vars else []),
        "-drive",
        f"format=raw,file=fat:rw:{out / 'esp'}",
        "-drive",
        f"format=raw,file={out / run_images[0]}",
        "-drive",
        f"if=ide,index=2,media=disk,format=raw,file={out / run_images[1]}",
        "-drive",
        f"if=none,format=raw,file={out / run_images[2]},id=vwfs0",
        "-device",
        "virtio-blk-pci,drive=vwfs0,id=vwfs,addr=0x6.0",
    ]
    b.run_target("run-qemu", qemu, deps=[esp_stamp, *run_images], description="QEMU")
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
