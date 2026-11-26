"""C example guest modules, each staged on the ESP under apps/."""

from wasmos_rules import guest, install, kernel_blob

# Examples laid out as <name>/<name>.c with <name>/linker.metadata.
SIMPLE = [
    "chardev_preempt",
    "console_claim_deny",
    "fs_leave_open",
    "fs_open_smoke",
    "fs_write_smoke",
    "gfx_mode",
    "gfx_smoke",
    "init_smoke",
    "inv_forge",
    "irq_route_allow",
    "irq_route_deny",
    "linmem_hold",
    "linmem_host_write",
    "menu_bar",
    "native_call_min",
    "native_call_smoke",
    "net_smoke",
    "net_tcp_server",
    "net_udp_echo",
    "phys_map_acpi",
    "phys_map_denied",
    "phys_map_refused",
    "surface_attach",
    "sv_probe",
    "wfs_write_smoke",
    "xfer_map_alias",
]


def build(b):
    # chardev_client ships as apps/chardevc.wap and is also embedded in the
    # kernel image as a blob of its bytecode.
    wap = guest(
        b,
        "chardev_client",
        [b.src("chardev_client/chardev_client.c")],
        manifest=b.src("chardev_client/linker.metadata"),
    )
    install(b, "apps/chardevc.wap", wap)
    b.exports.setdefault("kernel.blobs", []).append(
        kernel_blob(b, "chardev_client.wasm")
    )

    wap = guest(
        b,
        "hello_c",
        [b.src("hello/hello_c.c")],
        manifest=b.src("hello/linker.metadata"),
    )
    install(b, "apps/hello_c.wap", wap)

    wap = guest(
        b,
        "net_tcp_echo",
        [
            b.src("net_tcp_echo/net_tcp_echo.c"),
            b.root / "src/libsys/wasm/ipc_future_wasm.c",
        ],
        manifest=b.src("net_tcp_echo/linker.metadata"),
    )
    install(b, "apps/net_tcp_echo.wap", wap)

    # Built with the WebAssembly atomics feature: the module exists to run the
    # 0xFE atomic loads and stores (8, 16 and 32 bit) on both runtimes.
    wap = guest(
        b,
        "atomics_smoke",
        [b.src("atomics_smoke/atomics_smoke.c")],
        manifest=b.src("atomics_smoke/linker.metadata"),
        flags=["-matomics"],
    )
    install(b, "apps/atomics_smoke.wap", wap)

    for name in SIMPLE:
        wap = guest(
            b,
            name,
            [b.src(f"{name}/{name}.c")],
            manifest=b.src(f"{name}/linker.metadata"),
        )
        install(b, f"apps/{name}.wap", wap)
