"""wasm32 libc for guest modules: libc.a and crt1.o in the staged sysroot."""

# Everything under src/ except startup.c, which becomes crt1.o. script.c is
# included although only the CLI and the script executor use it: archive
# semantics leave it out of every module that does not reference it.
SOURCES = [
    "ctype.c",
    "ipc_managed.c",
    "math.c",
    "script.c",
    "spawn_info.c",
    "stdio.c",
    "stdlib.c",
    "string.c",
    "unistd.c",
]


def build(b):
    wasm32 = b.toolchain("wasm32")
    libdir = b.exports["sdk.libdir"]
    objs = wasm32.compile([b.src("src/" + s) for s in SOURCES])
    libc = wasm32.archive(f"{libdir}/libc.a", objs)
    crt1 = wasm32.compile(
        [b.src("src/startup.c")], obj_names={b.src("src/startup.c"): f"{libdir}/crt1.o"}
    )[0]
    b.exports["sdk.archives"] += [libc, crt1]
