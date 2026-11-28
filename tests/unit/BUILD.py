"""Host unit tests: kernel, libc, libsys and service logic compiled for the
build machine and run there (`ninja run-kernel-unit-tests`).

C/C++ test binaries are ordinary build edges, so they compile in parallel and
rebuild only when an input changes. The run target executes every suite in
order from the build directory and stops at the first failure. Suites that
compile and run in one step (Zig, Rust, AssemblyScript, Go) are steps of the
run target. See skills/wasmos-regression-test for how to add one.
"""

import platform
import subprocess
from pathlib import Path

from wasmos_rules import lang_available

C11 = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=c23-extensions"]

# Entry kinds. CC/CXX/SAN/SANXX build a binary (SAN and SANXX: sanitizer
# builds of C and C++, through the platform SDK compiler on macOS); RUN executes
# one; CHECK is a compile-only assertion run as a step.
CC, CXX, SAN, SANXX, RUN, CHECK = "cc", "cxx", "san", "sanxx", "run", "check"

ROOT = Path(__file__).resolve().parents[2]


def R(path):
    return str(ROOT / path)


_ARCH = platform.machine().lower()
# The native coroutine context switch the host-side native suites link.
HOST_ASM = R(
    "src/libsys/native/coroutine_native_aarch64.S"
    if _ARCH in ("arm64", "aarch64")
    else "src/libsys/native/coroutine_native_x86_64.S"
)

# The arch headers matching the host, for a suite whose kernel source reaches an
# arch inline (arch/arch_context.h selects on WASMOS_ARCH_AARCH64).
HOST_ARCH_DEFS = ["-DWASMOS_ARCH_AARCH64=1"] if _ARCH in ("arm64", "aarch64") else []

# The kernel's warp_setjmp/warp_longjmp for the host: the aarch64 assembly pair on
# an arm64 host, nothing extra on x86_64 (its header maps to clang builtins).
HOST_WARP_JMP = (
    [R("src/kernel/arch/aarch64/warp_jmp_aarch64.S")]
    if _ARCH in ("arm64", "aarch64")
    else []
)

# Linker flag that drops unreferenced sections (ld64 vs GNU/lld spelling).
DEAD_STRIP = "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections"

# The WARP allocation tracking table (src/kernel/warp/mem_utils_kernel.cpp) on
# the host, built against the vendored vb::MemUtils header; the plain and the
# ThreadSanitizer arm share it.
WARP_MEM_TRACKING = [
    "-std=c++17",
    "-Wall",
    "-Wno-unused-parameter",
    "-Wno-unused-const-variable",
    "-Wno-macro-redefined",
    "-Wno-c++20-extensions",
    "-pthread",
    "-DWASMOS_WASM_RUNTIME_WARP",
    "-DWASMOS_HOST_TEST_SMP=1",
    "-DVB_POSIX",
    "-DVB_WIN32_OR_POSIX",
    "-include",
    R("src/libc/include/wasmos_cast.h"),
    "-iquote",
    R("src/kernel"),
    "-iquote",
    R("src/kernel/include"),
    "-iquote",
    R("abi/generated/c"),
    "-iquote",
    R("tests/unit"),
    "-I",
    R("libs/warp"),
    R("tests/unit/test_warp_mem_tracking.cpp"),
    R("src/kernel/warp/mem_utils_kernel.cpp"),
]

# The WARP backend's freestanding C++ synchronization (src/kernel/warp/compat
# and warp/cxx_abi.cpp) on the host; the plain and the ThreadSanitizer arm
# share it.
WARP_COMPAT_SYNC = [
    *HOST_ARCH_DEFS,
    "-std=c++17",
    "-Wall",
    "-pthread",
    "-iquote",
    R("src/kernel"),
    "-iquote",
    R("src/kernel/include"),
    "-iquote",
    R("tests/unit"),
    R("tests/unit/test_warp_compat_sync.cpp"),
]

# The WARP compat <limits>/<type_traits>/<functional>, compiled as the kernel
# compiles WARP: C++14 with no host C++ library and the compat directory first.
WARP_COMPAT_TRAITS = [
    "-std=c++14",
    "-nostdinc++",
    "-Wall",
    "-I",
    R("src/kernel/warp/compat"),
    "-iquote",
    R("tests/unit"),
    R("tests/unit/test_warp_compat_traits.cpp"),
]

# Suites in run order.
# fmt: off
# The .wap parser (wasmos_app.c) compiled for real with what it links against:
# the subsystem registry, Ed25519 and the test signing key, runtime and slab
# stubs. Shared by every suite that parses packages.
# The kernel's mbedTLS subset (KERNEL_MBEDTLS in src/kernel/BUILD.py, which this
# list must match), compiled as the kernel compiles it: wasmos_app.c verifies
# RSA-PSS package signatures with it.
KERNEL_MBEDTLS = [R('libs/mbedtls/library/' + f) for f in (
    "asn1parse.c", "asn1write.c", "bignum.c", "bignum_core.c", "constant_time.c", "md.c",
    "oid.c", "pk.c", "pk_wrap.c", "pkparse.c", "platform.c", "platform_util.c", "rsa.c",
    "rsa_alt_helpers.c", "sha512.c",
)] + [R('src/kernel/crypto/kernel_mbedtls.c')]
KERNEL_MBEDTLS_FLAGS = ['-I', R('libs/mbedtls/include'), '-I', R('src/kernel/crypto'),
                        '-DMBEDTLS_CONFIG_FILE="kernel_mbedtls_config.h"',
                        '-Wno-unused-parameter', '-Wno-sign-compare']

WASMOS_APP_PARSER = [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/drivers/include'), "-I" + R('src/kernel/include'), "-I" + R('abi/generated/c'), "-I" + R('libs/wasm3/source'), "-I" + R('libs/monocypher/src'), "-I" + R('libs/monocypher/src/optional'), '-iquote', R('src/libc/include'), R('tests/unit/stubs_signing_pubkey.c'), R('libs/monocypher/src/monocypher.c'), R('libs/monocypher/src/optional/monocypher-ed25519.c'), R('tests/unit/stubs_app_runtime.c'), R('tests/unit/stubs_spinlock.c'), R('src/kernel/wasmos_app.c'), R('src/kernel/subsystem_registry.c'), R('src/kernel/hashmap.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('src/libc/src/string.c'), *KERNEL_MBEDTLS_FLAGS, *KERNEL_MBEDTLS]

TESTS = [
    (CC, 'test_list', [*C11, "-I" + R('src/kernel/include'), R('tests/unit/test_list.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c')]),
    (RUN, 'test_list', []),
    (CC, 'test_kmem', [*C11, "-I" + R('src/kernel/include'), R('tests/unit/test_kmem.c'), R('src/kernel/kmem.c')]),
    (RUN, 'test_kmem', []),
    (CC, 'test_hashmap', [*C11, "-I" + R('src/kernel/include'), R('tests/unit/test_hashmap.c'), R('src/kernel/hashmap.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c')]),
    (RUN, 'test_hashmap', []),
    (CC, 'test_subsystem_registry', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/kernel/include'), '-iquote', R('src/libc/include'), R('tests/unit/test_subsystem_registry.c'), R('tests/unit/stubs_spinlock.c'), R('src/kernel/subsystem_registry.c'), R('src/kernel/hashmap.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_subsystem_registry', []),
    (CC, 'test_wasmos_exec_format', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/drivers/include'), "-I" + R('src/kernel/include'), '-iquote', R('src/libc/include'), R('tests/unit/test_wasmos_exec_format.c'), R('tests/unit/stubs_spinlock.c'), R('src/kernel/wasmos_exec_format.c'), R('src/kernel/subsystem_registry.c'), R('src/kernel/hashmap.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wasmos_exec_format', []),
    (CC, 'test_wasmos_app_deps', [R('tests/unit/test_wasmos_app_deps.c'), *WASMOS_APP_PARSER]),
    (RUN, 'test_wasmos_app_deps', []),
    (CC, 'test_wasmos_app_aot', [R('tests/unit/test_wasmos_app_aot.c'), *WASMOS_APP_PARSER]),
    (RUN, 'test_wasmos_app_aot', []),
    (CC, 'test_wasmos_app_rsa', [R('tests/unit/test_wasmos_app_rsa.c'), *WASMOS_APP_PARSER]),
    (RUN, 'test_wasmos_app_rsa', []),
    (CC, 'test_wasmos_app_allow', [R('tests/unit/test_wasmos_app_allow.c'), *WASMOS_APP_PARSER]),
    (RUN, 'test_wasmos_app_allow', []),
    (CC, 'test_device_manager_rules', [*C11, "-I" + R('src/services/device_manager'), "-I" + R('src/libsys/wasm/include'), '-iquote', R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_device_manager_rules.c'), R('src/services/device_manager/device_manager_rules.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_device_manager_rules', []),
    (CC, 'test_device_manager_unit', [*C11, "-I" + R('src/services/device_manager'), "-I" + R('src/libsys/wasm/include'), '-iquote', R('tests/unit'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_device_manager_unit.c'), R('src/services/device_manager/device_manager_unit.c')]),
    (RUN, 'test_device_manager_unit', []),
    (CC, 'test_virtio_input_events', [*C11, "-I" + R('tests/unit'), "-I" + R('src/drivers/input/virtio_input'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_virtio_input_events.c'), R('src/drivers/input/virtio_input/virtio_input_events.c')]),
    (RUN, 'test_virtio_input_events', []),
    (CC, 'test_virtio_gpu_damage', [*C11, "-I" + R('tests/unit'), "-I" + R('src/drivers/gpu/virtio_gpu'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_virtio_gpu_damage.c'), R('src/drivers/gpu/virtio_gpu/virtio_gpu_damage.c')]),
    (RUN, 'test_virtio_gpu_damage', []),
    (CC, 'test_block_descriptor', [*C11, "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_block_descriptor.c')]),
    (RUN, 'test_block_descriptor', []),
    (CC, 'test_svc_name_from_mount', [*C11, "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_svc_name_from_mount.c')]),
    (RUN, 'test_svc_name_from_mount', []),
    (CC, 'test_device_manager_block_rules', [*C11, '-Wno-unused-function', '-ffunction-sections', '-fdata-sections', DEAD_STRIP, '-iquote', R('src/services/device_manager'), '-iquote', R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_device_manager_block_rules.c'), R('tests/unit/stubs_device_manager_host.c'), R('src/services/device_manager/device_manager_rules.c'), R('src/services/device_manager/device_manager_unit.c')]),
    (RUN, 'test_device_manager_block_rules', []),
    (CC, 'test_fs_manager_path', [*C11, "-I" + R('src/services/fs_manager'), R('tests/unit/test_fs_manager_path.c'), R('src/services/fs_manager/fs_manager_path.c')]),
    (RUN, 'test_fs_manager_path', []),
    (CC, 'test_fs_manager_backends', [*C11, "-I" + R('src/services/fs_manager'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), '-iquote', R('src/libsys/wasm/include'), R('tests/unit/test_fs_manager_backends.c'), R('src/services/fs_manager/fs_manager_backends.c')]),
    (RUN, 'test_fs_manager_backends', []),
    (CC, 'test_fs_manager_clients', [*C11, "-I" + R('src/services/fs_manager'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), '-iquote', R('src/libsys/wasm/include'), R('tests/unit/test_fs_manager_clients.c'), R('src/services/fs_manager/fs_manager_clients.c')]),
    (RUN, 'test_fs_manager_clients', []),
    (CC, 'test_fs_manager_pending', [*C11, "-I" + R('src/services/fs_manager'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), '-iquote', R('src/libsys/wasm/include'), R('tests/unit/test_fs_manager_pending.c'), R('src/services/fs_manager/fs_manager_pending.c')]),
    (RUN, 'test_fs_manager_pending', []),
    (CC, 'test_fs_manager_units', [*C11, "-I" + R('src/services/fs_manager'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), '-iquote', R('src/libsys/wasm/include'), R('tests/unit/test_fs_manager_units.c'), R('src/services/fs_manager/fs_manager_units.c')]),
    (RUN, 'test_fs_manager_units', []),
    (CC, 'test_cli_ls_order', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/services/cli'), R('tests/unit/test_cli_ls_order.c'), R('src/services/cli/cli_ls_order.c')]),
    (RUN, 'test_cli_ls_order', []),
    (CC, 'test_vt_outmux', [*C11, "-I" + R('src/services/vt'), R('tests/unit/test_vt_outmux.c'), R('src/services/vt/vt_outmux.c')]),
    (RUN, 'test_vt_outmux', []),
    (CC, 'test_logger_stage', [*C11, "-I" + R('src/services/logger'), R('tests/unit/test_logger_stage.c'), R('src/services/logger/logger_stage.c')]),
    (RUN, 'test_logger_stage', []),
    (CC, 'test_supervisor_unit', [*C11, "-I" + R('src/services/supervisor'), R('tests/unit/test_supervisor_unit.c'), R('src/services/supervisor/supervisor_unit.c')]),
    (RUN, 'test_supervisor_unit', []),
    (CC, 'test_fat_block', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/fat'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_fat_block.c'), R('src/drivers/fs/fat/fat_block.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_fat_block', []),
    (CC, 'test_fat_dir', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/fat'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), "-I" + R('src/libsys/wasm/include'), '-iquote', R('src/libc/include'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_fat_dir.c'), R('src/drivers/fs/fat/fat_dir.c'), R('src/drivers/fs/fat/fat_geom.c'), R('src/drivers/fs/fat/fat_alloc.c'), R('src/drivers/fs/fat/fat_name.c'), R('src/drivers/fs/fat/fat_file.c'), R('src/drivers/fs/fat/fat_util.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_fat_dir', []),
    (CC, 'test_fat_image', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/fat'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), "-I" + R('src/libsys/wasm/include'), '-iquote', R('src/libc/include'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_fat_image.c'), R('src/drivers/fs/fat/fat_dir.c'), R('src/drivers/fs/fat/fat_geom.c'), R('src/drivers/fs/fat/fat_alloc.c'), R('src/drivers/fs/fat/fat_name.c'), R('src/drivers/fs/fat/fat_file.c'), R('src/drivers/fs/fat/fat_util.c'), R('src/libc/src/string.c')]),
    (CC, 'test_wfs_format', ['-std=c11', '-Wall', '-Wextra', '-Werror', "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_format.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/drivers/fs/wfs/wfs_super.c')]),
    (RUN, 'test_wfs_format', []),
    (CC, 'test_wfs_mkfs', ['-std=c11', '-Wall', '-Wextra', '-Werror', "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_mkfs.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/drivers/fs/wfs/wfs_super.c')]),
    (RUN, 'test_wfs_mkfs', []),
    (CC, 'test_wfs_mount', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_mount.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_mount', []),
    (CC, 'test_wfs_extent', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_extent', []),
    (CC, 'test_wfs_namespace', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_namespace.c'), R('src/drivers/fs/wfs/wfs_namespace.c'), R('src/drivers/fs/wfs/wfs_dirent.c'), R('src/drivers/fs/wfs/wfs_truncate.c'), R('src/drivers/fs/wfs/wfs_write.c'), R('src/drivers/fs/wfs/wfs_read.c'), R('src/drivers/fs/wfs/wfs_path.c'), R('src/drivers/fs/wfs/wfs_dir.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('src/drivers/fs/wfs/wfs_alloc.c'), R('src/drivers/fs/wfs/wfs_bitmap.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_namespace', []),
    (CC, 'test_wfs_truncate', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_truncate.c'), R('src/drivers/fs/wfs/wfs_truncate.c'), R('src/drivers/fs/wfs/wfs_write.c'), R('src/drivers/fs/wfs/wfs_read.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('src/drivers/fs/wfs/wfs_alloc.c'), R('src/drivers/fs/wfs/wfs_bitmap.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_truncate', []),
    (CC, 'test_wfs_write', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_write.c'), R('src/drivers/fs/wfs/wfs_write.c'), R('src/drivers/fs/wfs/wfs_truncate.c'), R('src/drivers/fs/wfs/wfs_read.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('src/drivers/fs/wfs/wfs_alloc.c'), R('src/drivers/fs/wfs/wfs_bitmap.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_write', []),
    (CC, 'test_wfs_sync', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_sync.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_sync', []),
    (CC, 'test_wfs_fsck', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/utils/fsck_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_fsck.c'), R('src/utils/fsck_wfs/wfs_fsck.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_fsck', []),
    (CC, 'test_wfs_journal', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_journal.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_journal', []),
    (CC, 'test_wfs_recover', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_recover.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_namespace.c'), R('src/drivers/fs/wfs/wfs_dirent.c'), R('src/drivers/fs/wfs/wfs_path.c'), R('src/drivers/fs/wfs/wfs_dir.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('src/drivers/fs/wfs/wfs_alloc.c'), R('src/drivers/fs/wfs/wfs_bitmap.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_recover', []),
    (CC, 'test_wfs_alloc', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_alloc.c'), R('src/drivers/fs/wfs/wfs_alloc.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_bitmap.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_alloc', []),
    (CC, 'test_wfs_dirent', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_dirent.c'), R('src/drivers/fs/wfs/wfs_dirent.c'), R('src/drivers/fs/wfs/wfs_crc32c.c')]),
    (RUN, 'test_wfs_dirent', []),
    (CC, 'test_wfs_bitmap', [*C11, "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), R('tests/unit/test_wfs_bitmap.c'), R('src/drivers/fs/wfs/wfs_bitmap.c')]),
    (RUN, 'test_wfs_bitmap', []),
    (CC, 'test_wfs_dir', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_dir.c'), R('src/drivers/fs/wfs/wfs_dir.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_dir', []),
    (CC, 'test_wfs_populate', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_populate.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_dir.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_populate', []),
    (CC, 'test_wfs_read', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_read.c'), R('src/drivers/fs/wfs/wfs_read.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_dir.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_read', []),
    (CC, 'test_wfs_path', [*C11, "-I" + R('tests/unit'), "-I" + R('tests/unit/include'), "-I" + R('src/drivers/fs/wfs'), "-I" + R('src/tools/mkfs_wfs'), "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_wfs_path.c'), R('src/drivers/fs/wfs/wfs_path.c'), R('src/drivers/fs/wfs/wfs_mount.c'), R('src/drivers/fs/wfs/wfs_journal.c'), R('src/drivers/fs/wfs/wfs_sync.c'), R('src/drivers/fs/wfs/wfs_recover.c'), R('src/drivers/fs/wfs/wfs_dir.c'), R('src/drivers/fs/wfs/wfs_extent.c'), R('src/drivers/fs/wfs/wfs_extent_write.c'), R('tests/unit/stubs_wfs_block_server.c'), R('src/drivers/fs/wfs/wfs_ops.c'), R('src/drivers/fs/wfs/wfs_block.c'), R('src/drivers/fs/wfs/wfs_super.c'), R('src/drivers/fs/wfs/wfs_crc32c.c'), R('src/tools/mkfs_wfs/wfs_mkfs.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_wfs_path', []),
    (CC, 'test_irq_sharing', [*C11, "-I" + R('src/kernel/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_irq_sharing.c'), R('src/kernel/irq_sharing.c')]),
    (RUN, 'test_irq_sharing', []),
    (CC, 'test_msi_vectors', [*C11, "-I" + R('src/kernel/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_msi_vectors.c'), R('src/kernel/msi_vectors.c')]),
    (RUN, 'test_msi_vectors', []),
    (CC, 'test_xfer_buffer_object', [*C11, "-I" + R('src/kernel/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_xfer_buffer/xfer_buffer.c'), R('tests/unit/stubs_xfer_buffer_platform.c'), R('tests/unit/stubs_spinlock.c'), R('src/kernel/xfer_buffer/xfer_buffer.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c')]),
    (RUN, 'test_xfer_buffer_object', []),
    (CC, 'test_sched_runqueue_bias_on', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_runqueue.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_runqueue_bias_on', []),
    (CC, 'test_sched_runqueue_bias_off', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=0', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_runqueue.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_runqueue_bias_off', []),
    (CC, 'test_timer_tick', [*C11, '-DWASMOS_SMP=1', '-DWASMOS_HOST_TEST_SMP=1', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_timer_tick.c'), R('src/kernel/timer.c')]),
    (RUN, 'test_timer_tick', []),
    (CC, 'test_sched_event', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_EVENT_TEST_SEAMS=1', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_event.c'), R('src/kernel/sched_event.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_event', []),
    (CC, 'test_sched_concurrency_smp1', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=1', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_smp1', []),
    (CC, 'test_sched_concurrency_smp4', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=4', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_smp4', []),
    (CC, 'test_sched_concurrency_smp6', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=6', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_smp6', []),
    (CC, 'test_sched_concurrency_smp8', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=8', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_smp8', []),
    (CC, 'test_sched_concurrency_smp16', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=16', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_smp16', []),
    (SAN, 'test_sched_concurrency_tsan', ['-std=c11', '-Wall', '-pthread', '-fsanitize=thread', '-g', '-O1', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=4', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_tsan', []),
    (SAN, 'test_sched_concurrency_asan', ['-std=c11', '-Wall', '-pthread', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-O1', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SCHED_CALLER_CPU_BIAS=1', '-DWASMOS_TEST_NCPU=8', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_sched_concurrency.c'), R('src/kernel/sched_thread.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_sched_concurrency_asan', []),
    (CC, 'test_ipc', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_IPC_TEST_SEAMS=1', '-DWASMOS_IDTABLE_TEST_SEAMS=1', '-DWASMOS_POLL_TEST_SEAMS=1', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_ipc.c'), R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_ipc', []),
    (CC, 'test_idtable', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_IDTABLE_TEST_SEAMS=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_idtable.c'), R('src/kernel/idtable.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_idtable', []),
    (CC, 'test_syscall_ipc', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SYSCALL_TEST_SEAMS=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_syscall_ipc.c'), R('src/kernel/syscall.c'), R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_syscall_ipc', []),
    (CC, 'test_libsys_event_loop', [*C11, '-iquote', R('src/libsys/wasm/include'), '-iquote', R('src/libc/include'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_libsys_event_loop.c')]),
    (RUN, 'test_libsys_event_loop', []),
    (CC, 'test_libc_script_runner', [*C11, '-D_DEFAULT_SOURCE', '-iquote', R('src/libc/include'), '-iquote', R('tests/unit'), R('tests/unit/test_libc_script_runner.c'), R('src/libc/src/script.c')]),
    (RUN, 'test_libc_script_runner', []),
    (CC, 'test_libsys_svc_watch', [*C11, '-iquote', R('src/libsys/wasm/include'), '-iquote', R('src/libc/include'), '-iquote', R('src/drivers/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_libsys_svc_watch.c'), R('src/libsys/wasm/svc_watch_wasm.c'), R('src/libsys/wasm/coroutine_wasm.c')]),
    (RUN, 'test_libsys_svc_watch', []),
    (CXX, 'test_warp_link_ipc.o', ['-std=c++17', '-Wall', '-Wno-unused-parameter', '-DWASMOS_HOST_TEST_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-c', R('src/kernel/warp/link_ipc.cpp')]),
    (CC, 'test_wasm3_link_ipc.o', [*C11, '-Wno-unused-parameter', '-DWASMOS_HOST_TEST_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-c', R('src/kernel/wasm3/link_ipc.c')]),
    (CXX, 'test_hostcall_ipc', ['-Wall', '-Wno-unused-parameter', '-DWASMOS_HOST_TEST_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-x', 'c++', R('tests/unit/test_hostcall_ipc.cpp'), '-x', 'none', 'test_warp_link_ipc.o', 'test_wasm3_link_ipc.o', '-x', 'c', R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/capability.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c'), '-x', 'none']),
    (RUN, 'test_hostcall_ipc', []),
    (CXX, 'test_warp_link_rtc', ['-std=c++17', '-Wall', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_warp_link_rtc.cpp'), R('src/kernel/warp/link_rtc.cpp')]),
    (RUN, 'test_warp_link_rtc', []),
    (CXX, 'test_hostcall_dma', ['-Wall', '-Wno-unused-parameter', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), '-x', 'c++', R('tests/unit/test_hostcall_dma.cpp'), R('src/kernel/warp/link_dma.cpp'), '-x', 'c', R('src/kernel/wasm3/link_dma.c'), R('src/kernel/capability.c'), R('src/kernel/policy.c'), R('src/kernel/hostcall_dma.c'), R('src/kernel/hostcall_value.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_kpanic.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_hostcall_dma', []),
    (CC, 'test_process_lifecycle_smp2', [*C11, '-pthread', '-D_POSIX_C_SOURCE=200809L', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_PROCESS_TEST_SEAMS=1', '-DWASMOS_TEST_NCPU=2', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_process_lifecycle.c'), R('tests/unit/stubs_process_platform.c'), R('src/kernel/process.c'), R('src/kernel/thread.c'), R('src/kernel/sched_thread.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('src/kernel/idtable.c'), R('src/kernel/sched_event.c'), R('src/kernel/futex.c'), R('tests/unit/stubs_spinlock.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_kpanic.c')]),
    (RUN, 'test_process_lifecycle_smp2', []),
    (CC, 'test_process_lifecycle_smp4', [*C11, '-pthread', '-D_POSIX_C_SOURCE=200809L', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_PROCESS_TEST_SEAMS=1', '-DWASMOS_TEST_NCPU=4', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_process_lifecycle.c'), R('tests/unit/stubs_process_platform.c'), R('src/kernel/process.c'), R('src/kernel/thread.c'), R('src/kernel/sched_thread.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('src/kernel/idtable.c'), R('src/kernel/sched_event.c'), R('src/kernel/futex.c'), R('tests/unit/stubs_spinlock.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_kpanic.c')]),
    (RUN, 'test_process_lifecycle_smp4', []),
    (CC, 'test_process_lifecycle_smp8', [*C11, '-pthread', '-D_POSIX_C_SOURCE=200809L', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_PROCESS_TEST_SEAMS=1', '-DWASMOS_TEST_NCPU=8', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_process_lifecycle.c'), R('tests/unit/stubs_process_platform.c'), R('src/kernel/process.c'), R('src/kernel/thread.c'), R('src/kernel/sched_thread.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('src/kernel/idtable.c'), R('src/kernel/sched_event.c'), R('src/kernel/futex.c'), R('tests/unit/stubs_spinlock.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_kpanic.c')]),
    (RUN, 'test_process_lifecycle_smp8', []),
    (CHECK, None, ['--target=wasm32', '-nostdlib', '-fsyntax-only', "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_go_abi_sizes.c')]),
    (CC, 'test_block_buffer', [*C11, '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_block_buffer.c'), R('src/kernel/block_buffer.c')]),
    (RUN, 'test_block_buffer', []),
    (CC, 'test_hostcall_value', [*C11, '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_hostcall_value.c'), R('src/kernel/hostcall_value.c')]),
    (RUN, 'test_hostcall_value', []),
    (CC, 'test_phys_map', [*C11, '-iquote', R('src/kernel/include'), R('tests/unit/test_phys_map.c'), R('src/kernel/phys_map.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_phys_map', []),
    (CC, 'test_kenv', [*C11, '-pthread', '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_kenv.c'), R('src/kernel/kenv.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_kenv', []),
    (CC, 'test_kmsg', [*C11, '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_kmsg.c'), R('src/kernel/kmsg.c')]),
    (RUN, 'test_kmsg', []),
    (CC, 'test_mm_regions', [*C11, '-D_POSIX_C_SOURCE=200809L', '-Wno-unused-parameter', '-pthread', *HOST_ARCH_DEFS, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_mm_regions.c'), R('src/kernel/memory.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c'), R('tests/unit/stubs_kpanic.c')]),
    (RUN, 'test_mm_regions', []),
    (CC, 'test_warp_r3_hostcall_args', [*C11, '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), '-I', R('tests/unit/include'), R('tests/unit/test_warp_r3_hostcall_args.c')]),
    (RUN, 'test_warp_r3_hostcall_args', []),
    (CC, 'test_warp_r3_hostcall_args_aarch64', [*C11, '-DWASMOS_ARCH_AARCH64', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), '-I', R('tests/unit/include'), R('tests/unit/test_warp_r3_hostcall_args.c')]),
    (RUN, 'test_warp_r3_hostcall_args_aarch64', []),
    (CC, 'test_keyring_pem', [*C11, '-I', R('src/services/keyring'), '-I', R('libs/monocypher/src'), R('tests/unit/test_keyring_pem.c'), R('src/services/keyring/keyring_pem.c'), R('libs/monocypher/src/monocypher.c')]),
    (RUN, 'test_keyring_pem', []),
    (CC, 'test_physmem', [*C11, '-DWASMOS_PHYSMEM_TEST_SEAMS=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_physmem.c'), R('src/kernel/physmem.c'), R('tests/unit/stubs_kpanic.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_physmem', []),
    (CC, 'test_linmem_slots', [*C11, '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_linmem_slots.c'), R('src/kernel/linmem_slots.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_linmem_slots', []),
    (CXX, 'test_warp_aot_host_mem', ['-std=c++14', '-Wall', '-Wno-unused-parameter', '-Wno-macro-redefined', '-Wno-c++20-extensions', '-Wno-deprecated', '-DJIT_TARGET_X86_64', '-DVB_POSIX', '-DVB_WIN32_OR_POSIX', '-DNVALGRIND', '-I', R('libs/warp'), '-I', R('libs/warp/src'), R('tests/unit/test_warp_aot_host_mem.cpp'), R('src/tools/warp_aot/host_mem_utils.cpp')]),
    (RUN, 'test_warp_aot_host_mem', []),
    (CC, 'test_pm_shutdown', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), R('tests/unit/test_pm_shutdown.c'), R('src/kernel/process_manager_shutdown.c'), R('src/kernel/shutdown_order.c')]),
    (RUN, 'test_pm_shutdown', []),
    (CC, 'test_pm_caps', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), R('tests/unit/test_pm_caps.c'), R('src/kernel/process_manager_caps.c')]),
    (RUN, 'test_pm_caps', []),
    (CC, 'test_capability', [*C11, '-pthread', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_capability.c'), R('src/kernel/capability.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_kpanic.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_capability', []),
    (CC, 'test_io_region', [*C11, '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('tests/unit/shadow_noports'), '-iquote', R('src/kernel'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_io_region.c'), R('src/kernel/io_region.c'), R('src/kernel/capability.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_kpanic.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_io_region', []),
    (CC, 'test_framebuffer_geometry', [*C11, '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_framebuffer_geometry.c'), R('src/kernel/framebuffer.c')]),
    (RUN, 'test_framebuffer_geometry', []),
    (CC, 'test_efi_runtime_map', [*C11, '-iquote', R('src/boot'), '-iquote', R('src/kernel/include'), '-iquote', R('tests/unit'), R('tests/unit/test_efi_runtime_map.c')]),
    (RUN, 'test_efi_runtime_map', []),
    (CC, 'test_efi_connect', [*C11, '-iquote', R('src/boot'), '-iquote', R('src/kernel/include'), '-iquote', R('tests/unit'), R('tests/unit/test_efi_connect.c')]),
    (RUN, 'test_efi_connect', []),
    (CC, 'test_efi_loader_alloc', [*C11, '-iquote', R('src/boot'), '-iquote', R('src/kernel/include'), '-iquote', R('tests/unit'), R('tests/unit/test_efi_loader_alloc.c')]),
    (RUN, 'test_efi_loader_alloc', []),
    (CC, 'test_device_window', [*C11, '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_device_window.c'), R('src/kernel/device_window.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_device_window', []),
    (CC, 'test_pci_config', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('tests/unit/shadow_io'), '-iquote', R('tests/unit/include'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_pci_config.c'), R('src/kernel/pci_config.c'), R('src/kernel/arch/x86_64/pci_cfg_x86_64.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_pci_config', []),
    (CC, 'test_hostcall_buffer', [*C11, '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_hostcall_buffer.c'), R('src/kernel/hostcall_buffer.c')]),
    (RUN, 'test_hostcall_buffer', []),
    (CC, 'test_poll', [*C11, '-iquote', R('src/kernel/include'), '-iquote', R('abi/generated/c'), R('tests/unit/test_poll.c'), R('src/kernel/poll.c')]),
    (RUN, 'test_poll', []),
    (CC, 'test_futex', [*C11, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_FUTEX_TEST_SEAMS=1', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_futex.c'), R('src/kernel/futex.c'), R('src/kernel/sched_event.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_futex', []),
    (CC, 'test_ipc_concurrency_t4', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_TEST_NTHREADS=4', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_ipc_concurrency.c'), R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_ipc_concurrency_t4', []),
    (CC, 'test_ipc_concurrency_t8', [*C11, '-pthread', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_TEST_NTHREADS=8', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_ipc_concurrency.c'), R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_ipc_concurrency_t8', []),
    (SAN, 'test_ipc_concurrency_tsan', ['-std=c11', '-Wall', '-pthread', '-fsanitize=thread', '-g', '-O1', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_TEST_NTHREADS=4', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_ipc_concurrency.c'), R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_ipc_concurrency_tsan', []),
    (SAN, 'test_ipc_concurrency_asan', ['-std=c11', '-Wall', '-pthread', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-O1', '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_TEST_NTHREADS=8', '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), R('tests/unit/test_ipc_concurrency.c'), R('src/kernel/ipc.c'), R('src/kernel/idtable.c'), R('tests/unit/stubs_kpanic.c'), R('src/kernel/poll.c'), R('src/kernel/sched_event.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('tests/unit/stubs_spinlock.c')]),
    (RUN, 'test_ipc_concurrency_asan', []),
    (CC, 'test_warp_mem_tracking_spin.o', [*C11, '-O1', '-iquote', R('src/kernel/include'), '-c', R('tests/unit/stubs_spinlock.c')]),
    (CXX, 'test_warp_mem_tracking', [*WARP_MEM_TRACKING, '-O1', 'test_warp_mem_tracking_spin.o']),
    (RUN, 'test_warp_mem_tracking', []),
    (SAN, 'test_warp_mem_tracking_spin_tsan.o', [*C11, '-fsanitize=thread', '-g', '-O1', '-iquote', R('src/kernel/include'), '-c', R('tests/unit/stubs_spinlock.c')]),
    (SANXX, 'test_warp_mem_tracking_tsan', [*WARP_MEM_TRACKING, '-fsanitize=thread', '-g', '-O1', 'test_warp_mem_tracking_spin_tsan.o']),
    (RUN, 'test_warp_mem_tracking_tsan', []),
    (CXX, 'test_warp_compat_sync', [*WARP_COMPAT_SYNC, '-O1', 'test_warp_mem_tracking_spin.o']),
    (RUN, 'test_warp_compat_sync', []),
    (SANXX, 'test_warp_compat_sync_tsan', [*WARP_COMPAT_SYNC, '-fsanitize=thread', '-g', '-O1', 'test_warp_mem_tracking_spin_tsan.o']),
    (RUN, 'test_warp_compat_sync_tsan', []),
    (CXX, 'test_warp_compat_traits', WARP_COMPAT_TRAITS),
    (RUN, 'test_warp_compat_traits', []),
    (CC, 'test_warp_jmp', [*C11, '-O2', *HOST_ARCH_DEFS, '-iquote', R('src/kernel/include'), '-iquote', R('tests/unit'), R('tests/unit/test_warp_jmp.c'), *HOST_WARP_JMP]),
    (RUN, 'test_warp_jmp', []),
    (CC, 'test_warp_exception_checkpoint', [*C11, '-Wno-unused-parameter', *HOST_ARCH_DEFS, '-DWASMOS_HOST_TEST_SMP=1', '-DWASMOS_SMP=1', '-DWASMOS_WASM_RUNTIME_WARP', '-include', R('src/libc/include/wasmos_cast.h'), '-iquote', R('src/kernel/include'), '-iquote', R('libs/wasm3/source'), '-iquote', R('abi/generated/c'), '-iquote', R('tests/unit'), '-iquote', R('src/kernel'), R('tests/unit/test_warp_exception_checkpoint.c'), R('src/kernel/warp/exception_checkpoint.c'), R('tests/unit/stubs_kpanic.c')]),
    (RUN, 'test_warp_exception_checkpoint', []),
    (CC, 'test_warp_driver_ring3_call_policy', [*C11, "-I" + R('src/kernel/include'), R('tests/unit/test_warp_driver_ring3_call_policy.c'), R('src/kernel/warp_driver_ring3_call_policy.c')]),
    (RUN, 'test_warp_driver_ring3_call_policy', []),
    (CC, 'test_kernel_sync_primitives', [*C11, '-DWASMOS_TEST_USE_REAL_SPINLOCK_DECLS', '-pthread', "-I" + R('tests/unit/include'), "-I" + R('src/kernel/include'), R('tests/unit/test_kernel_sync_primitives.c'), R('src/kernel/sync_mutex.c'), R('src/kernel/sync_semaphore.c')]),
    (RUN, 'test_kernel_sync_primitives', []),
    (CC, 'test_user_mutex', [*C11, "-I" + R('src/kernel/include'), R('tests/unit/test_user_mutex.c')]),
    (RUN, 'test_user_mutex', []),
    (CC, 'test_libc_stdio', [*C11, "-I" + R('src/libc/include'), "-I" + R('src/libc/src'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_libc_stdio.c'), R('src/libc/src/stdio.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_libc_stdio', []),
    (CC, 'test_libc_math', [*C11, '-fno-builtin', "-I" + R('src/libc/include'), "-I" + R('src/libc/src'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_libc_math.c'), R('src/libc/src/math.c')]),
    (RUN, 'test_libc_math', []),
    (CC, 'test_libui_shim_alloc', [*C11, '-fno-builtin', '-DWASMOS_LIBUI_SHIM_TEST_SEAMS=1', "-I" + R('src/libc/include'), "-I" + R('src/libui/include'), "-I" + R('src/drivers/include'), R('tests/unit/test_libui_shim_alloc.c'), R('src/libui/zig/libui_shim.c')]),
    (RUN, 'test_libui_shim_alloc', []),
    (CC, 'test_libc_ipc_call', [*C11, '-fno-builtin', "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), '-iquote', R('tests/unit'), R('tests/unit/test_libc_ipc_call.c')]),
    (RUN, 'test_libc_ipc_call', []),
    (CC, 'test_libc_string', [*C11, '-fno-builtin', "-I" + R('src/libc/include'), "-I" + R('src/libc/src'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_libc_string.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_libc_string', []),
    (CC, 'test_libc_startup_argv', [*C11, "-I" + R('src/libc/include'), "-I" + R('src/libc/src'), "-I" + R('src/drivers/include'), "-I" + R('tests/unit'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_libc_startup_argv.c'), R('src/libc/src/spawn_info.c')]),
    (RUN, 'test_libc_startup_argv', []),
    (CC, 'test_libc_stdlib', [*C11, '-Wno-error=implicit-function-declaration', '-Wno-sign-compare', '-D__builtin_wasm_memory_size(x)=((int)(0))', '-D__builtin_wasm_memory_grow(x,y)=((int)(-1))', "-I" + R('src/libc/include'), "-I" + R('src/libc/src'), '-include', R('src/libc/include/wasmos_cast.h'), R('tests/unit/test_libc_stdlib.c'), R('tests/unit/stubs_wasm_stdlib.c'), R('src/libc/src/stdlib.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_libc_stdlib', []),
    (CC, 'test_vring', [*C11, "-I" + R('src/libsys/wasm/include'), R('tests/unit/test_vring.c')]),
    (RUN, 'test_vring', []),
    (CC, 'test_virtio_pci_caps', [*C11, "-I" + R('src/libsys/wasm/include'), '-iquote', R('tests/unit'), R('tests/unit/test_virtio_pci_caps.c')]),
    (RUN, 'test_virtio_pci_caps', []),
    (CC, 'test_ringbuf', [*C11, '-pthread', "-I" + R('src/libsys/wasm/include'), "-I" + R('src/drivers/include'), R('tests/unit/test_ringbuf.c')]),
    (RUN, 'test_ringbuf', []),
    (CC, 'test_libui_key_decode', [*C11, "-I" + R('src/libui/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), "-I" + R('abi/generated/c'), R('tests/unit/test_libui_key_decode.c')]),
    (RUN, 'test_libui_key_decode', []),
    (CC, 'test_heap_native_alloc.o', ['-std=c11', '-Wno-error=c23-extensions', '-Dmalloc=hn_malloc', '-Dfree=hn_free', '-Dcalloc=hn_calloc', '-Drealloc=hn_realloc', "-I" + R('src/drivers/include'), '-c', R('src/libsys/native/heap_native.c')]),
    (CC, 'test_heap_native', [*C11, "-I" + R('src/drivers/include'), R('tests/unit/test_heap_native.c'), 'test_heap_native_alloc.o']),
    (RUN, 'test_heap_native', []),
    (CC, 'test_net_socket', [*C11, "-I" + R('src/libsys/wasm/include'), "-I" + R('src/drivers/include'), "-I" + R('src/services/net_stack'), R('tests/unit/test_net_socket.c'), R('src/services/net_stack/socket.c')]),
    (RUN, 'test_net_socket', []),
    (CC, 'test_net_ifcfg', [*C11, "-I" + R('src/services/net_stack'), R('tests/unit/test_net_ifcfg.c'), R('src/services/net_stack/net_stack_ifcfg.c')]),
    (RUN, 'test_net_ifcfg', []),
    (CC, 'test_service_class_registry', [*C11, "-I" + R('src/kernel/include'), '-iquote', R('src/libc/include'), R('tests/unit/test_service_class_registry.c'), R('src/kernel/service_class_registry.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_service_class_registry', []),
    (CC, 'test_service_name_watch', [*C11, "-I" + R('src/kernel/include'), '-iquote', R('src/libc/include'), R('tests/unit/test_service_name_watch.c'), R('src/kernel/service_name_watch.c'), R('src/kernel/list.c'), R('src/kernel/list_linked.c'), R('src/kernel/list_array_chunk.c'), R('src/kernel/kmem.c'), R('tests/unit/stubs_slab.c'), R('src/libc/src/string.c')]),
    (RUN, 'test_service_name_watch', []),
    (CC, 'test_shutdown_order', [*C11, "-I" + R('src/kernel/include'), '-iquote', R('tests/unit'), R('tests/unit/test_shutdown_order.c'), R('src/kernel/shutdown_order.c')]),
    (RUN, 'test_shutdown_order', []),
    (CC, 'test_wasm_coroutine_rust_core.o', [*C11, "-I" + R('src/libsys/wasm/include'), '-c', R('src/libsys/wasm/coroutine_wasm.c')]),
    (RUN, 'test_wasm_coroutine_rust', ['--test-threads=1']),
    (RUN, 'test_ipc_call_rust', ['--test-threads=1']),
    (CC, 'test_wasm_coroutine', [*C11, "-I" + R('src/libsys/wasm/include'), "-I" + R('src/libc/include'), "-I" + R('src/drivers/include'), R('tests/unit/test_wasm_coroutine.c'), R('src/libsys/wasm/coroutine_wasm.c'), R('src/libsys/wasm/ipc_future_wasm.c')]),
    (RUN, 'test_wasm_coroutine', []),
    (CC, 'test_native_coroutine', [*C11, "-I" + R('src/libsys/native/include'), R('tests/unit/test_native_coroutine.c'), R('src/libsys/native/coroutine_native.c'), HOST_ASM]),
    (RUN, 'test_native_coroutine', []),
    (CC, 'test_native_ipc_future', [*C11, "-I" + R('src/libsys/native/include'), "-I" + R('src/drivers/include'), "-I" + R('src/libc/include'), R('tests/unit/test_native_ipc_future.c'), R('tests/unit/stubs_native_libsys.c'), R('src/libsys/native/coroutine_native.c'), R('src/libsys/native/ipc_future_native.c'), R('src/libsys/native/service_runtime_native.c'), R('src/libsys/native/zig/libsys_native.c'), HOST_ASM]),
    (RUN, 'test_native_ipc_future', []),
    (CC, 'test_native_svc_watch', [*C11, "-I" + R('src/libsys/native/include'), "-I" + R('src/drivers/include'), "-I" + R('src/libc/include'), R('tests/unit/test_native_svc_watch.c'), R('tests/unit/stubs_native_libsys.c'), R('src/libsys/native/coroutine_native.c'), R('src/libsys/native/svc_watch_native.c'), R('src/libsys/native/zig/libsys_native.c'), HOST_ASM]),
    (RUN, 'test_native_svc_watch', []),
]
# fmt: on


def _host_sdk_compiler(driver="clang"):
    """Sanitizer builds use the platform SDK's clang (or clang++, as `driver`)
    on macOS, whose runtime libraries match the system; elsewhere the
    configured compiler."""
    if platform.system() == "Darwin":
        try:
            sdk = subprocess.run(
                ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
            return ["xcrun", driver, "-isysroot", sdk]
        except (OSError, subprocess.CalledProcessError):
            pass
    return None


def _inputs(b, args):
    """Files a test command reads: every listed source, plus the headers AND
    sources under every include directory it names (multi-source commands get
    no depfile). Sources count because a suite may #include a .c to reach its
    static functions (test_device_manager_block_rules includes
    device_manager.c); without them an edit to that file leaves the suite
    running the old code."""
    files, dirs = [], []
    for a in args:
        p = Path(a[2:] if a.startswith("-I") else a)
        if not p.is_absolute():
            continue
        if p.is_file():
            files.append(p)
        elif p.is_dir():
            dirs.append(p)
    for d in dirs:
        files += b.glob("**/*.h", root=d)
        files += b.glob("**/*.c", root=d)
    return files


SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".S")


def _index_sources(b, cc, args):
    """One compile_commands.json entry per test-side source (under tests/),
    with the test's flags and none of its other sources. The src/ files a test
    compiles are indexed by the build that really compiles them."""
    sources = [a for a in args if a.endswith(SOURCE_SUFFIXES) and Path(a).is_absolute()]
    flags = [
        a
        for a in args
        if a not in sources
        and a not in ("-x", "c", "c++", "none")
        and not a.endswith(".o")
    ]
    sources = [a for a in sources if Path(a).is_relative_to(ROOT / "tests")]
    for src in sources:
        lang = ["-x", "c++"] if src.endswith((".cc", ".cpp", ".cxx")) else []
        b.index(src, [*cc, *flags, *lang])


def zig_test(
    b, label, modules, root_module, c_sources=(), c_flags=(), deps=(), module_flags=()
):
    """`zig test` for the host, as a run step. Zig compiles and runs in one
    command; its caches live in the build directory. Zig scopes -I flags by
    their position relative to -M, so `module_flags` follow the module
    definitions."""
    target = []
    if platform.system() == "Darwin":
        arch = "aarch64" if _ARCH in ("arm64", "aarch64") else "x86_64"
        # A pinned deployment target links the installed SDK's libSystem.
        target = ["-target", f"{arch}-macos.15.0"]
    argv = [
        b.tools["zig"],
        "test",
        *target,
        *c_sources,
        *c_flags,
        "--cache-dir",
        f"zig-{label}-cache",
        "--global-cache-dir",
        "zig-global-cache",
    ]
    for dep in deps:
        argv += ["--dep", dep]
    argv.append(f"-Mroot={root_module}")
    for name, path in modules.items():
        argv.append(f"-M{name}={path}")
    argv += list(module_flags)
    if platform.system() == "Darwin":
        argv.append("-lSystem")
    return argv


def as_test(b, name, files):
    """An AssemblyScript suite: stage its files flat (asc resolves imports
    beside the entry), compile, and run under node."""
    stage = f"as_{name}_src"
    entry = Path(files[-1]).name
    steps = [["mkdir", "-p", stage]] + [["cp", f, f"{stage}/"] for f in files]
    extra = []
    if name == "test_as_coroutine_transform":
        extra = ["--transform", R("tools/as_coroutine_transform.mjs")]
    steps.append(
        [
            b.tools["asc"],
            f"{stage}/{entry}",
            "--target",
            "release",
            "-O",
            "--runtime",
            "stub",
            *extra,
            "--outFile",
            f"{name}.wasm",
        ]
    )
    steps.append(["node", R("tests/unit/as/run_as_test.mjs"), f"{name}.wasm", name])
    return steps


def build(b):
    clang, clangxx = b.tools["clang"], b.tools["clangxx"]
    san_cc = _host_sdk_compiler() or [clang]
    san_cxx = _host_sdk_compiler("clang++") or [clangxx]
    steps, built = [], []
    unit = ROOT / "tests/unit"

    # The Rust coroutine binding's host test links the core object the table
    # builds (test_wasm_coroutine_rust_core.o); the table's RUN entry runs it.
    if lang_available(b, "rust"):
        rust_test = "test_wasm_coroutine_rust"
        b.command(
            [rust_test],
            [
                b.tools["rustc"],
                "--test",
                unit / "test_wasm_coroutine.rs",
                "-C",
                "link-arg=test_wasm_coroutine_rust_core.o",
                "-o",
                rust_test,
            ],
            inputs=[unit / "test_wasm_coroutine.rs"],
            implicit=["test_wasm_coroutine_rust_core.o"],
            description=f"RUSTC[unit] {rust_test}",
        )
        built.append(rust_test)
        # ipc::call in src/libc/rust/wasmos.rs against the fake mailbox the test
        # exports; the binding uses raw identifiers, so it needs edition 2018+.
        ipc_call_test = "test_ipc_call_rust"
        b.command(
            [ipc_call_test],
            [
                b.tools["rustc"],
                "--edition",
                "2021",
                "--test",
                unit / "test_ipc_call.rs",
                "-o",
                ipc_call_test,
            ],
            inputs=[unit / "test_ipc_call.rs"],
            implicit=[
                ROOT / "src/libc/rust/wasmos.rs",
                ROOT / "src/libc/rust/coroutine.rs",
            ],
            description=f"RUSTC[unit] {ipc_call_test}",
        )
        built.append(ipc_call_test)
    for kind, name, args in TESTS:
        if kind in (CC, CXX, SAN, SANXX):
            cc = {CC: [clang], CXX: [clangxx], SAN: san_cc, SANXX: san_cxx}[kind]
            # Objects an earlier entry built (e.g. test_warp_link_ipc.o) are
            # named by bare build-directory path.
            local = [a for a in args if a in built]
            b.command(
                [name],
                cc + args + ["-o", name],
                implicit=_inputs(b, args) + local,
                description=f"CC[unit] {name}",
            )
            built.append(name)
            # Indexed with the configured clang: `xcrun clang` is not a
            # compiler path a tool can replay.
            _index_sources(b, [clangxx] if kind in (CXX, SANXX) else [clang], args)
        elif kind == RUN:
            # A suite whose toolchain is unavailable built no binary.
            if name in built:
                steps.append([f"./{name}", *args])
        elif kind == CHECK:
            steps.append([clang, *args])
            _index_sources(b, [clang], args)

    libc, libsys_wasm = ROOT / "src/libc", ROOT / "src/libsys/wasm"
    libsys_native, drivers = ROOT / "src/libsys/native", ROOT / "src/drivers"

    if lang_available(b, "as") and b.tools.get("asc"):
        shuffle = unit / "as/shuffle.ts"
        as_lib = libc / "assemblyscript"
        steps += as_test(
            b,
            "test_as_coroutine",
            [as_lib / "coroutine.ts", shuffle, unit / "test_as_coroutine.ts"],
        )
        steps += as_test(
            b,
            "test_as_eventloop",
            [
                as_lib / "coroutine.ts",
                as_lib / "eventloop.ts",
                ROOT / "abi/generated/assemblyscript/wasmos_imports.ts",
                shuffle,
                unit / "test_as_eventloop.ts",
            ],
        )
        steps += as_test(
            b,
            "test_as_coroutine_transform",
            [as_lib / "coroutine.ts", shuffle, unit / "test_as_coroutine_transform.ts"],
        )
        steps += as_test(
            b,
            "test_as_serial_name",
            [
                drivers / "tty/serial/serial_name.ts",
                shuffle,
                unit / "test_as_serial_name.ts",
            ],
        )
        steps += as_test(
            b,
            "test_as_uart_window",
            [
                drivers / "tty/serial/uart_window.ts",
                shuffle,
                unit / "test_as_uart_window.ts",
            ],
        )
        steps += as_test(
            b,
            "test_as_uart",
            [drivers / "tty/serial/uart.ts", shuffle, unit / "test_as_uart.ts"],
        )
        steps += as_test(
            b,
            "test_as_rtc_time",
            [drivers / "rtc/core/rtc_time.ts", shuffle, unit / "test_as_rtc_time.ts"],
        )

    # The Go binding runs on wasm32: Go models every record as an opaque blob
    # sized for wasm32, so a host build would exercise a different ABI.
    sdk = b.exports["sdk"]
    if b.tools.get("tinygo"):
        stage = "go_coroutine_test_src"
        steps += [
            ["mkdir", "-p", stage],
            [
                "cp",
                libc / "go/coroutine.go",
                libc / "go/wasmos.go",
                unit / "test_wasm_coroutine.go",
                f"{stage}/",
            ],
            [f"{sdk['bin']}/wasmos-tinygo", "--emit-target-json=wasmos-tinygo.json"],
            [
                "env",
                "GO111MODULE=off",
                b.tools["tinygo"],
                "build",
                "-target=wasmos-tinygo.json",
                "-opt=z",
                "-no-debug",
                "-panic=trap",
                "-scheduler=none",
                "-gc=leaking",
                "-o",
                "test_wasm_coroutine_go.wasm",
                f"./{stage}",
            ],
            [
                "node",
                unit / "go/run_go_test.mjs",
                "test_wasm_coroutine_go.wasm",
                "test_wasm_coroutine_go",
            ],
        ]

    if lang_available(b, "zig"):
        pm = drivers / "block/partition"
        fixtures = unit / "fixtures_disk_images.zig"
        steps.append(
            zig_test(
                b,
                "wasm-coroutine",
                {"coroutine": libc / "zig/coroutine.zig"},
                unit / "test_wasm_coroutine.zig",
                c_sources=[libsys_wasm / "coroutine_wasm.c"],
                c_flags=[f"-I{libsys_wasm / 'include'}"],
                deps=["coroutine"],
            )
        )
        steps.append(
            zig_test(
                b,
                "ipc-call",
                {"wasmos": libc / "zig/wasmos.zig"},
                unit / "test_ipc_call.zig",
                # wasmos.zig declares its host calls `extern "wasmos"`; on an ELF
                # host that names a dynamic library, which Zig accepts only for
                # PIC. The test exports the symbols itself, so nothing is
                # loaded at run time.
                c_flags=["-fPIC"],
                deps=["wasmos"],
            )
        )
        steps.append(
            zig_test(
                b,
                "partition-table",
                {
                    "partition_table": pm / "partition_table.zig",
                    "fixtures_disk_images": fixtures,
                },
                unit / "test_partition_table.zig",
                deps=["partition_table", "fixtures_disk_images"],
            )
        )
        for label, mod, src in (
            ("partition-reconcile", "reconcile", pm / "reconcile.zig"),
            ("partition-mounts", "mounts", pm / "mounts.zig"),
            ("partition-inflight", "inflight", pm / "inflight.zig"),
            ("partition-backends", "backends", pm / "backends.zig"),
            ("fs-tmpfs", "fs_tmpfs_store", drivers / "fs/tmpfs/fs_tmpfs_store.zig"),
        ):
            test = {
                "reconcile": "test_partition_reconcile.zig",
                "mounts": "test_partition_mounts.zig",
                "inflight": "test_partition_inflight.zig",
                "backends": "test_partition_backends.zig",
                "fs_tmpfs_store": "test_fs_tmpfs_store.zig",
            }[mod]
            steps.append(zig_test(b, label, {mod: src}, unit / test, deps=[mod]))
        # recognise.zig imports its siblings by bare name, so they are staged
        # into one directory beside it.
        stage = "zig_recognise_test_src"
        vm = ROOT / "src/services/volume_manager"
        steps += [
            ["mkdir", "-p", stage],
            [
                "cp",
                vm / "recognise.zig",
                vm / "recognise_fat.zig",
                vm / "recognise_wfs.zig",
                pm / "partition_table.zig",
                ROOT / "abi/generated/zig/wasmos_constants.zig",
                f"{stage}/",
            ],
        ]
        steps.append(
            zig_test(
                b,
                "recognise",
                {
                    "recognise": f"{stage}/recognise.zig",
                    "fixtures_disk_images": fixtures,
                },
                unit / "test_fs_recognise.zig",
                deps=["recognise", "fixtures_disk_images"],
            )
        )
        native_inc = [f"-I{libsys_native / 'include'}", f"-I{drivers / 'include'}"]
        steps.append(
            zig_test(
                b,
                "native-coroutine",
                {"libsys": libsys_native / "zig/libsys.zig"},
                unit / "test_native_coroutine.zig",
                c_sources=[
                    libsys_native / "coroutine_native.c",
                    libsys_native / "ipc_future_native.c",
                    libsys_native / "service_runtime_native.c",
                    libsys_native / "zig/libsys_native.c",
                    unit / "stubs_native_libsys.c",
                    HOST_ASM,
                ],
                c_flags=native_inc + [f"-I{libc / 'include'}"],
                deps=["libsys"],
                module_flags=native_inc,
            )
        )

    # The coroutine fixture through host WARP: needs a host WARP backend, so
    # it runs on x86_64 hosts of a WARP configuration only.
    warp_host = b.exports.get("warp_host")
    warp_test = unit / "test_warp_wasm_coroutine.cpp"
    fixture_src = unit / "wasm_coroutine_warp_fixture.c"
    if warp_host:
        b.index(warp_test, [clangxx, *warp_host["flags"]])
    b.index(
        fixture_src,
        [
            clang,
            "--target=wasm32",
            "-Oz",
            "-nostdlib",
            "-ffreestanding",
            f"-I{libsys_wasm / 'include'}",
            f"-I{libc / 'include'}",
        ],
    )
    if warp_host and _ARCH in ("x86_64", "amd64"):
        fixture = "test_wasm_coroutine_fixture.wasm"
        fixture_srcs = [
            unit / "wasm_coroutine_warp_fixture.c",
            libsys_wasm / "coroutine_wasm.c",
            libc / "src/string.c",
            libc / "src/ctype.c",
        ]
        b.command(
            [fixture],
            [
                clang,
                "--target=wasm32",
                "-Oz",
                "-nostdlib",
                "-ffreestanding",
                "-Wl,--no-entry",
                "-Wl,--strip-all",
                "-Wl,--export=wasmos_coroutine_wasm_test",
                f"-I{libsys_wasm / 'include'}",
                f"-I{libc / 'include'}",
                *fixture_srcs,
                "-o",
                fixture,
            ],
            inputs=fixture_srcs,
            description=f"CC[wasm32] {fixture}",
        )
        objs = b.toolchain("host").compile(
            warp_host["sources"] + [unit / "test_warp_wasm_coroutine.cpp"],
            cxxflags=warp_host["flags"],
            subdir="obj/warp_host_test",
        )
        test = "test_warp_wasm_coroutine"
        b.command(
            [test],
            [clangxx, *objs, "-o", test],
            inputs=objs,
            description=f"LINK[host] {test}",
        )
        built += [test, fixture]
        steps.append([f"./{test}", fixture])

    b.run_target(
        "run-kernel-unit-tests",
        steps,
        deps=[*built, *sdk["deps"]],
        cwd=b.out,
        description="host unit tests",
    )
    b.alias("unit-test-binaries", *built)

    # fs_fat against images real formatters built. The driver script exits 77
    # when no formatter is installed, which is a skip, not a failure.
    b.run_target(
        "run-fat-image-tests",
        [
            "sh",
            "-c",
            f"sh {R('scripts/run_fat_image_test.sh')}; rc=$?; "
            '[ "$rc" -eq 77 ] && exit 0; exit "$rc"',
        ],
        deps=["test_fat_image"],
        env={
            "WASMOS_FAT_IMAGE_TEST_BIN": b.out / "test_fat_image",
            "WASMOS_FAT_IMAGE_WORK": b.out / "fat-images",
        },
        description="fs_fat against formatter-built FAT images",
    )
