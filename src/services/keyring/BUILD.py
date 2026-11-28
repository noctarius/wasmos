"""keyring: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "keyring",
        [
            b.root / "src/libsys/wasm/service_async_entry_wasm.c",
            b.src("keyring.c"),
            b.src("keyring_pem.c"),
        ],
        flags=b.exports["monocypher"]["include"],
        libs=["-lmonocypher"],
    )
    install(b, "system/services/keyring.wap", wap)
