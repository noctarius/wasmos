"""C example guest modules, each staged on the ESP under apps/."""

from wasmos_rules import guest, install, kernel_blob, pack

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
    "priv_deputy",
    "priv_probe",
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

    # One module packed four ways for tests/test_warp_aot.py: signed with an
    # AOT binary for every architecture; signed with binaries only for the
    # architectures other than the tree's; unsigned, with the tree's own; and
    # signed with a key the kernel does not trust.
    wap = guest(
        b,
        "aot_fat",
        [b.src("aot_probe/aot_probe.c")],
        manifest=b.src("aot_probe/linker.metadata"),
    )
    install(b, "apps/aotfat.wap", wap)
    if wap is not None:
        probe = "aot_fat.wasm"
        wap = pack(
            b, "aot_foreign", probe, b.src("aot_probe/foreign.metadata"), aot="foreign"
        )
        install(b, "apps/aotforgn.wap", wap)
        wap = pack(
            b,
            "aot_unsigned",
            probe,
            b.src("aot_probe/unsigned.metadata"),
            aot=True,
            sign=False,
        )
        install(b, "apps/aotunsig.wap", wap)
        # Signed, but with a key this kernel does not trust: a fixed test seed,
        # not a secret. The kernel must refuse to start it.
        b.write_file("signing/untrusted-test-seed.hex", "5a" * 32 + "\n")
        wap = pack(
            b,
            "aot_untrusted",
            probe,
            b.src("aot_probe/untrusted.metadata"),
            aot=True,
            sign=b.out / "signing/untrusted-test-seed.hex",
        )
        install(b, "apps/aotuntru.wap", wap)

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

    # An app using the SDK's Monocypher library.
    wap = guest(
        b,
        "crypto_smoke",
        [b.src("crypto_smoke/crypto_smoke.c")],
        manifest=b.src("crypto_smoke/linker.metadata"),
        flags=b.exports["monocypher"]["include"],
        libs=["-lmonocypher"],
    )
    install(b, "apps/crypto_smoke.wap", wap)

    # priv_probe's bytecode packed three more ways for the signature policy
    # (tests/test_signed_packages.py): without a signature; signed and then
    # altered by one byte in its payload, so the signature no longer matches;
    # and signed with the build's app key.
    probe_wasm = "priv_probe.wasm"
    unsigned = pack(
        b,
        "priv_unsigned",
        probe_wasm,
        b.src("priv_probe/linker.metadata"),
        aot=False,
        sign=False,
    )
    install(b, "apps/priv_unsigned.wap", unsigned)
    signed = pack(
        b, "priv_resigned", probe_wasm, b.src("priv_probe/linker.metadata"), aot=False
    )
    tamper = b.root / "scripts/tamper_byte.py"
    b.command(
        ["priv_tampered.wap"],
        [b.tools["python"], tamper, signed, "priv_tampered.wap"],
        inputs=[signed],
        implicit=[tamper],
        description="TAMPER priv_tampered.wap",
    )
    install(b, "apps/priv_tampered.wap", "priv_tampered.wap")
    # Signed with the build's app key: trusted, but a publisher's, so its
    # svc.control is refused while it still starts.
    publisher = pack(
        b,
        "priv_publisher",
        probe_wasm,
        b.src("priv_probe/linker.metadata"),
        aot=False,
        sign=b.exports["signing.app_seed"],
    )
    install(b, "apps/priv_publisher.wap", publisher)

    # Two RSA keys, generated once per build directory: the trusted one's public
    # half is installed in the keyring directory, the other's nowhere. A package
    # signed with each (RSASSA-PSS, SHA-512) starts as a publisher's and is
    # refused, respectively (tests/test_signed_packages.py). Test keys, not
    # secrets: a build that ships the examples trusts this one.
    packer = b.exports["packer"]
    for key, bits in (("rsa-test", 3072), ("rsa-untrusted", 2048)):
        b.command(
            [f"signing/{key}.key.pem"],
            [f"./{packer}", "--genkey-rsa", str(bits), f"signing/{key}.key.pem"],
            implicit=[packer],
            description=f"KEY signing/{key}.key.pem",
        )
    b.command(
        ["signing/rsa-test.pem"],
        [
            f"./{packer}",
            "--rsa-pubkey-pem",
            "signing/rsa-test.key.pem",
            "signing/rsa-test.pem",
        ],
        inputs=["signing/rsa-test.key.pem"],
        implicit=[packer],
        description="KEY signing/rsa-test.pem",
    )
    install(b, "system/keys/rsa-test.pem", b.out / "signing/rsa-test.pem")
    for name, key in (
        ("priv_rsa", "rsa-test"),
        ("priv_rsa_untrusted", "rsa-untrusted"),
    ):
        wap = pack(
            b,
            name,
            probe_wasm,
            b.src("priv_probe/linker.metadata"),
            aot=False,
            sign=b.out / f"signing/{key}.key.pem",
        )
        install(b, f"apps/{name}.wap", wap)

    # The "start anyway" fixtures (tests/test_trust.py): hello_c signed with the
    # untrusted Ed25519 test seed, which `trust allow` lets start; the untrusted
    # RSA key's public half, which `trust add-key` adds; and the trust utility
    # without keyring.admin, whose requests the keyring refuses.
    b.write_file("signing/untrusted-test-seed.hex", "5a" * 32 + "\n")
    wap = pack(
        b,
        "hello_untrusted",
        "hello_c.wasm",
        b.src("trust_fixtures/hello_untrusted.metadata"),
        aot=False,
        sign=b.out / "signing/untrusted-test-seed.hex",
    )
    install(b, "apps/hello_untrusted.wap", wap)
    b.command(
        ["signing/rsa-untrusted.pem"],
        [
            f"./{packer}",
            "--rsa-pubkey-pem",
            "signing/rsa-untrusted.key.pem",
            "signing/rsa-untrusted.pem",
        ],
        inputs=["signing/rsa-untrusted.key.pem"],
        implicit=[packer],
        description="KEY signing/rsa-untrusted.pem",
    )
    install(b, "apps/rsa-untrusted.pem", b.out / "signing/rsa-untrusted.pem")
    wap = pack(
        b,
        "trust_noadmin",
        "trust.wasm",
        b.src("trust_fixtures/trust_noadmin.metadata"),
        aot=False,
    )
    install(b, "apps/trust_noadmin.wap", wap)

    for name in SIMPLE:
        wap = guest(
            b,
            name,
            [b.src(f"{name}/{name}.c")],
            manifest=b.src(f"{name}/linker.metadata"),
        )
        install(b, f"apps/{name}.wap", wap)
