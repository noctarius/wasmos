"""gfx_compositor: native ring-3 Zig service (static x86_64 ELF)."""

from wasmos_rules import install, native, zig_object

# Shared flags for the C objects linked beside the Zig object.
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
    options = b.write_file(
        "gfx_compositor_build_options.zig",
        f"pub const gfx_trace: bool = {'true' if b.cfg.WASMOS_TRACE else 'false'};\n",
    )
    zig = zig_object(
        b,
        "gfx_compositor_zig.o",
        b.src("gfx_compositor.zig"),
        includes=[root, b.dir, drivers_inc, libc_inc, libsys / "include"],
        modules={
            "libsys": (libsys / "zig/libsys.zig", [drivers_inc, libsys / "include"]),
            "build_options": (options, []),
        },
        implicit=[
            *b.glob("*.zig"),
            *b.glob("*.h"),
            *b.glob("zig/*.zig", root=libsys),
            *b.glob("*.h", root=drivers_inc),
            *b.glob("wasmos/*.h", root=libc_inc),
        ],
    )
    tc = b.toolchain("native")
    sys_inc = [f"-I{drivers_inc}", f"-I{libc_inc}", f"-I{libsys / 'include'}"]
    objs = [zig]
    objs += tc.compile(
        [libsys / "zig/libsys_native.c"],
        flags=CFLAGS + sys_inc,
        subdir="obj/gfx_compositor",
    )
    objs += tc.compile(
        [root / "src/libc/src/ctype.c", root / "src/libc/src/string.c"],
        flags=CFLAGS + [f"-I{libc_inc}"],
        subdir="obj/gfx_compositor",
    )
    objs += tc.compile(
        [libsys / "svc_watch_native.c", libsys / "coroutine_native.c"],
        flags=CFLAGS + sys_inc,
        subdir="obj/gfx_compositor",
    )
    objs += tc.compile(
        [libsys / "coroutine_native_x86_64.S"], subdir="obj/gfx_compositor"
    )
    wap = native(b, "gfx_compositor", objs, entry="initialize")
    install(b, "system/services/gfxcomp.wap", wap)
