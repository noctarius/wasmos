"""framebuffer: native ring-3 driver (static ELF for the target architecture)."""

from wasmos_rules import install, native

CFLAGS = [
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-Oz",
    "-Wall",
    "-Wextra",
]


def build(b):
    fb = b.root / "src/drivers/gpu/framebuffer"
    flags = CFLAGS + [
        f"-I{b.root / 'src/drivers/include'}",
        f"-I{fb}",
        "-include",
        str(b.root / "src/libc/include/wasmos_cast.h"),
    ]
    objs = b.toolchain("native").compile(
        [b.src("framebuffer_native.c"), b.src("fbtext_server.c"), b.src("render.c")],
        flags=flags,
        subdir="obj/framebuffer",
    )
    wap = native(
        b,
        "framebuffer",
        objs,
        entry="initialize",
        image_base="0x10000000",
        elf="framebuffer_native.elf",
    )
    install(b, "system/drivers/framebuf.wap", wap)
