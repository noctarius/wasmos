"""net_stack: native ring-3 TCP/IP + TLS service (lwIP, mbedTLS; static ELF)."""

from toolchains import arch
from wasmos_rules import native

# lwIP core in link order.
LWIP = [
    "core/init.c",
    "core/def.c",
    "core/dns.c",
    "core/inet_chksum.c",
    "core/ip.c",
    "core/mem.c",
    "core/memp.c",
    "core/netif.c",
    "core/pbuf.c",
    "core/raw.c",
    "core/stats.c",
    "core/sys.c",
    "core/altcp.c",
    "core/altcp_alloc.c",
    "core/altcp_tcp.c",
    "core/tcp.c",
    "core/tcp_in.c",
    "core/tcp_out.c",
    "core/timeouts.c",
    "core/udp.c",
    "core/ipv4/acd.c",
    "core/ipv4/autoip.c",
    "core/ipv4/dhcp.c",
    "core/ipv4/etharp.c",
    "core/ipv4/icmp.c",
    "core/ipv4/igmp.c",
    "core/ipv4/ip4_frag.c",
    "core/ipv4/ip4.c",
    "core/ipv4/ip4_addr.c",
    "netif/ethernet.c",
]
ALTCP_TLS = [
    "apps/altcp_tls/altcp_tls_mbedtls.c",
    "apps/altcp_tls/altcp_tls_mbedtls_mem.c",
]
SERVICE = ["net_stack.c", "socket.c", "net_stack_ifcfg.c", "port.c"]
LIBSYS = [
    "coroutine_native.c",
    "heap_native.c",
    "ipc_future_native.c",
    "service_runtime_native.c",
    "service_async_entry_native.c",
]

BASE = [
    "-ffreestanding",
    "-nostdlibinc",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fno-builtin",
    "-nostdlib",
    "-O2",
]
# lwIP and mbedTLS are vendored subtrees and are compiled as they are.
VENDOR_WARN = [
    "-Wno-error",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
    "-Wno-address",
    "-Wno-sign-compare",
]
MBEDTLS_CONFIG = '-DMBEDTLS_CONFIG_FILE="net_stack_mbedtls_config.h"'


def build(b):
    root = b.root
    lwip, mbedtls = root / "libs/lwip/src", root / "libs/mbedtls"
    libc_inc, libsys = root / "src/libc/include", root / "src/libsys/native"
    includes = [
        f"-I{lwip / 'include'}",
        f"-I{b.dir}",
        f"-I{libc_inc}",
        f"-I{root / 'src/drivers/include'}",
        f"-I{root / 'src/libsys/wasm/include'}",
        f"-I{libsys / 'include'}",
        f"-I{mbedtls / 'include'}",
        MBEDTLS_CONFIG,
    ]
    freestanding = f"-I{b.dir / 'freestanding'}"
    tc = b.toolchain("native")

    def cc(sources, flags):
        return tc.compile(sources, flags=flags, subdir="obj/net_stack")

    objs = cc([lwip / s for s in LWIP], BASE + VENDOR_WARN + includes)
    objs += cc(
        b.glob("library/*.c", root=mbedtls),
        BASE
        + VENDOR_WARN
        + [
            freestanding,
            f"-I{mbedtls / 'include'}",
            f"-I{b.dir}",
            f"-I{libc_inc}",
            MBEDTLS_CONFIG,
        ],
    )
    objs += cc(
        [lwip / s for s in ALTCP_TLS],
        BASE
        + VENDOR_WARN
        + [
            f"-I{b.dir / 'lwip_mbedtls_shim'}",
            freestanding,
            *includes,
            "-include",
            str(b.src("net_stack_mbedtls3_compat.h")),
        ],
    )
    objs += cc(b.srcs(*SERVICE), BASE + VENDOR_WARN + includes)
    objs += cc([libsys / s for s in LIBSYS], BASE + includes)
    objs += cc([libsys / arch(b.cfg).coroutine_asm], [])
    objs += cc(
        [libsys / "zig/libsys_native.c"],
        BASE
        + [
            f"-I{root / 'src/drivers/include'}",
            f"-I{libc_inc}",
            f"-I{libsys / 'include'}",
        ],
    )
    objs += cc(
        [root / "src/libc/src/string.c", root / "src/libc/src/ctype.c"],
        BASE + [f"-I{libc_inc}"],
    )
    native(b, "net_stack", objs, entry="async_initialize")
