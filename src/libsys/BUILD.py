"""wasm32 libsys: the guest service runtime (coroutines, IPC futures, async
entries) as libsys.a in the staged sysroot."""

SOURCES = [
    "coroutine_wasm.c",
    "ipc_future_wasm.c",
    "service_runtime_wasm.c",
    "svc_watch_wasm.c",
]


def build(b):
    wasm32 = b.toolchain("wasm32")
    # Entry shims and Go glue are compiled per module by the drivers, not
    # archived; index them so tools see them with the archive's flags.
    for src in b.glob("wasm/*.c"):
        if src.name not in SOURCES:
            b.index(src, [wasm32.tc.cc, *wasm32.tc.cflags])
    objs = wasm32.compile([b.src("wasm/" + s) for s in SOURCES])
    b.exports["sdk.archives"].append(
        wasm32.archive(f"{b.exports['sdk.libdir']}/libsys.a", objs)
    )
