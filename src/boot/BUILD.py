"""UEFI loader: BOOTX64.EFI / BOOTAA64.EFI (PE/COFF, linked by lld-link
through clang)."""

from toolchains import arch


def build(b):
    efi = b.toolchain("efi")
    flags = [f"-I{b.root / 'src/kernel/include'}", f"-I{b.dir}"]
    # The firmware clock (Kconfig WASMOS_RTC_EFI) needs the runtime services
    # mapped where the kernel can call them; the loader does that only then,
    # because SetVirtualAddressMap cannot be undone.
    if b.cfg.WASMOS_RTC_EFI:
        flags.append("-DWASMOS_EFI_RUNTIME=1")
    objs = efi.compile([b.src("boot.c")], flags=flags)
    out = efi.link(arch(b.cfg).efi_boot_name, objs)
    b.alias("bootloader", out, default=True)
    b.exports["bootloader"] = out
