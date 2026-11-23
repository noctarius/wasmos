"""font_service: native ring-3 Zig TTF service (static ELF for the target architecture)."""

from wasmos_rules import install, native, zig_object

CFLAGS = [
    "-ffreestanding",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fno-builtin",
    "-nostdlib",
    "-O2",
]


def build(b):
    root = b.root
    drivers_inc, libc_inc = root / "src/drivers/include", root / "src/libc/include"
    libsys = root / "src/libsys/native"
    stb = root / "libs/stb"
    zig = zig_object(
        b,
        "font_service_zig.o",
        b.src("font_service.zig"),
        includes=[root, stb, b.dir, drivers_inc, libc_inc, libsys / "include"],
        modules={
            "libsys": (libsys / "zig/libsys.zig", [drivers_inc, libsys / "include"])
        },
        implicit=[
            *b.glob("*.zig"),
            *b.glob("*.h"),
            *b.glob("zig/*.zig", root=libsys),
            *b.glob("*.h", root=drivers_inc),
            *b.glob("wasmos/*.h", root=libc_inc),
            # Reached through wasmos_driver_abi.h and libsys/zig/c_abi.zig.
            *b.glob("*.h", root=root / "abi/generated/c"),
            *b.glob("include/wasmos/*.h", root=libsys),
            stb / "stb_truetype.h",
        ],
    )
    tc = b.toolchain("native")
    objs = [zig]
    objs += tc.compile(
        [libsys / "zig/libsys_native.c"],
        flags=CFLAGS + [f"-I{drivers_inc}", f"-I{libc_inc}", f"-I{libsys / 'include'}"],
        subdir="obj/font_service",
    )
    objs += tc.compile(
        [b.src("stb_truetype_impl.c")],
        flags=CFLAGS
        + [f"-I{root}", f"-I{stb}", f"-I{b.dir}", f"-I{drivers_inc}", f"-I{libc_inc}"],
        subdir="obj/font_service",
    )
    objs += tc.compile(
        [
            root / "src/libc/src/ctype.c",
            root / "src/libc/src/string.c",
            root / "src/libc/src/math.c",
        ],
        flags=CFLAGS + [f"-I{libc_inc}"],
        subdir="obj/font_service",
    )
    wap = native(b, "font_service", objs, entry="initialize")
    install(b, "system/services/fontsvc.wap", wap)
