"""UEFI loader: BOOTX64.EFI / BOOTAA64.EFI (PE/COFF, linked by lld-link
through clang)."""

from toolchains import arch


def build(b):
    efi = b.toolchain("efi")
    objs = efi.compile(
        [b.src("boot.c")], flags=[f"-I{b.root / 'src/kernel/include'}", f"-I{b.dir}"]
    )
    out = efi.link(arch(b.cfg).efi_boot_name, objs)
    b.alias("bootloader", out, default=True)
    b.exports["bootloader"] = out
