"""UEFI loader: BOOTX64.EFI (PE/COFF, linked by lld-link through clang)."""


def build(b):
    efi = b.toolchain("efi")
    objs = efi.compile(
        [b.src("boot.c")], flags=[f"-I{b.root / 'src/kernel/include'}", f"-I{b.dir}"]
    )
    out = efi.link("BOOTX64.EFI", objs)
    b.alias("bootloader", out, default=True)
    b.exports["bootloader"] = out
