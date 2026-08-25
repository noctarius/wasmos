cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strip host toolchain flags that break freestanding builds on macOS.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(ENV{CFLAGS} "")
  set(ENV{CXXFLAGS} "")
  set(ENV{ASMFLAGS} "")
  set(ENV{LDFLAGS} "")
  set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "Disable -arch injection" FORCE)
  set(CMAKE_OSX_SYSROOT "" CACHE PATH "Disable macOS sysroot injection" FORCE)
  set(CMAKE_C_FLAGS_INIT "" CACHE STRING "Reset initial C flags" FORCE)
  set(CMAKE_ASM_FLAGS_INIT "" CACHE STRING "Reset initial ASM flags" FORCE)
  execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE MACOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "Disable -arch injection for freestanding builds" FORCE)
endif ()

set(BUILD_DIR ${CMAKE_BINARY_DIR})
set(BOOT_DIR ${CMAKE_SOURCE_DIR}/src/boot)
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/src/kernel)
set(LIBC_DIR ${CMAKE_SOURCE_DIR}/src/libc)
set(LIBSYS_DIR ${CMAKE_SOURCE_DIR}/src/libsys)
set(LIBSYS_WASM_DIR ${LIBSYS_DIR}/wasm)
set(LIBSYS_NATIVE_DIR ${LIBSYS_DIR}/native)
set(WASM3_DIR ${CMAKE_SOURCE_DIR}/libs/wasm3)
set(WARP_DIR  ${CMAKE_SOURCE_DIR}/libs/warp)
set(DRIVER_WASM_DIR ${CMAKE_SOURCE_DIR}/src/drivers)
set(EXAMPLES_C_DIR ${CMAKE_SOURCE_DIR}/examples/c)
set(EXAMPLES_GO_DIR ${CMAKE_SOURCE_DIR}/examples/go)
set(EXAMPLES_RUST_DIR ${CMAKE_SOURCE_DIR}/examples/rust)
set(EXAMPLES_AS_DIR ${CMAKE_SOURCE_DIR}/examples/assemblyscript)
set(EXAMPLES_ZIG_DIR ${CMAKE_SOURCE_DIR}/examples/zig)
set(SERVICES_DIR ${CMAKE_SOURCE_DIR}/src/services)
set(USERFS_DIR ${CMAKE_SOURCE_DIR}/userfs)
# A WFS volume for the guest to mount. Unlike the two FAT disks it cannot be a
# QEMU synthetic FAT directory, so it is a raw image mkfs_wfs builds from
# scripts/wfs. --uuid is pinned so the image is byte-identical between builds and
# a change to it shows up as a diff rather than as noise.
set(WFS_IMAGE ${BUILD_DIR}/wfs.img)
set(WFS_CONTENT_DIR ${CMAKE_SOURCE_DIR}/scripts/wfs)
set(WFS_IMAGE_UUID 5746532d-7465-4573-742d-766f6c756d65)

set(WASM_CHARDEV_DRIVER_SRC ${DRIVER_WASM_DIR}/chardev/chardev_server.c)
set(WASM_CHARDEV_DRIVER_WASM ${BUILD_DIR}/chardev_server.wasm)
set(WASM_CHARDEV_SERVER_APP ${BUILD_DIR}/chardev_server.wap)
set(WASM_CHARDEV_CLIENT_SRC ${EXAMPLES_C_DIR}/chardev_client/chardev_client.c)
set(WASM_CHARDEV_CLIENT_WASM ${BUILD_DIR}/chardev_client.wasm)
set(WASM_CHARDEV_CLIENT_BLOB ${BUILD_DIR}/chardev_client_blob.o)
set(WASM_CHARDEV_CLIENT_APP ${BUILD_DIR}/chardev_client.wap)
set(WASM_CHARDEV_PREEMPT_SRC ${EXAMPLES_C_DIR}/chardev_preempt/chardev_preempt.c)
set(WASM_CHARDEV_PREEMPT_WASM ${BUILD_DIR}/chardev_preempt.wasm)
set(WASM_CHARDEV_PREEMPT_APP ${BUILD_DIR}/chardev_preempt.wap)
set(WASM_INIT_SMOKE_SRC ${EXAMPLES_C_DIR}/init_smoke/init_smoke.c)
set(WASM_INIT_SMOKE_WASM ${BUILD_DIR}/init_smoke.wasm)
set(WASM_INIT_SMOKE_APP ${BUILD_DIR}/init_smoke.wap)
set(WASM_FS_OPEN_SMOKE_SRC ${EXAMPLES_C_DIR}/fs_open_smoke/fs_open_smoke.c)
set(WASM_FS_OPEN_SMOKE_WASM ${BUILD_DIR}/fs_open_smoke.wasm)
set(WASM_FS_OPEN_SMOKE_APP ${BUILD_DIR}/fs_open_smoke.wap)
set(WASM_FS_WRITE_SMOKE_SRC ${EXAMPLES_C_DIR}/fs_write_smoke/fs_write_smoke.c)
set(WASM_FS_WRITE_SMOKE_WASM ${BUILD_DIR}/fs_write_smoke.wasm)
set(WASM_FS_WRITE_SMOKE_APP ${BUILD_DIR}/fs_write_smoke.wap)
set(WASM_NATIVE_CALL_MIN_SRC ${EXAMPLES_C_DIR}/native_call_min/native_call_min.c)
set(WASM_NATIVE_CALL_MIN_WASM ${BUILD_DIR}/native_call_min.wasm)
set(WASM_NATIVE_CALL_MIN_APP ${BUILD_DIR}/native_call_min.wap)
set(WASM_NATIVE_CALL_SMOKE_SRC ${EXAMPLES_C_DIR}/native_call_smoke/native_call_smoke.c)
set(WASM_NATIVE_CALL_SMOKE_WASM ${BUILD_DIR}/native_call_smoke.wasm)
set(WASM_NATIVE_CALL_SMOKE_APP ${BUILD_DIR}/native_call_smoke.wap)
set(WASM_WAMOS_SCRIPT_SRC ${SERVICES_DIR}/wasmos_script/wamos_script.c)
set(WASM_WAMOS_SCRIPT_WASM ${BUILD_DIR}/wamos_script.wasm)
set(WASM_WAMOS_SCRIPT_APP ${BUILD_DIR}/wamos_script.wap)
set(WASM_WAMOS_SCRIPT_BROKER_SRC ${SERVICES_DIR}/wasmos_script_broker/wamos_script_broker.c)
set(WASM_WAMOS_SCRIPT_BROKER_WASM ${BUILD_DIR}/wamos_script_broker.wasm)
set(WASM_WAMOS_SCRIPT_BROKER_APP ${BUILD_DIR}/wamos_script_broker.wap)
set(WAMOS_SCRIPT_DATA_SRC ${SERVICES_DIR}/wasmos_script/hello.rc)
set(WAMOS_SCRIPT_DATA_BIN ${BUILD_DIR}/hello.rc)
set(WASM_GFX_SMOKE_SRC ${EXAMPLES_C_DIR}/gfx_smoke/gfx_smoke.c)
set(WASM_GFX_SMOKE_WASM ${BUILD_DIR}/gfx_smoke.wasm)
set(WASM_GFX_SMOKE_APP ${BUILD_DIR}/gfx_smoke.wap)
set(WASM_IRQ_ROUTE_DENY_SRC ${EXAMPLES_C_DIR}/irq_route_deny/irq_route_deny.c)
set(WASM_IRQ_ROUTE_DENY_WASM ${BUILD_DIR}/irq_route_deny.wasm)
set(WASM_IRQ_ROUTE_DENY_APP ${BUILD_DIR}/irq_route_deny.wap)
set(WASM_IRQ_ROUTE_ALLOW_SRC ${EXAMPLES_C_DIR}/irq_route_allow/irq_route_allow.c)
set(WASM_IRQ_ROUTE_ALLOW_WASM ${BUILD_DIR}/irq_route_allow.wasm)
set(WASM_IRQ_ROUTE_ALLOW_APP ${BUILD_DIR}/irq_route_allow.wap)
set(WASM_SHMEM_OWNER_SRC ${EXAMPLES_C_DIR}/shmem_owner/shmem_owner.c)
set(WASM_SHMEM_OWNER_WASM ${BUILD_DIR}/shmownr.wasm)
set(WASM_SHMEM_OWNER_APP ${BUILD_DIR}/shmownr.wap)
set(WASM_SHMEM_TARGET_SRC ${EXAMPLES_C_DIR}/shmem_target/shmem_target.c)
set(WASM_SHMEM_TARGET_WASM ${BUILD_DIR}/shmtgt.wasm)
set(WASM_SHMEM_TARGET_APP ${BUILD_DIR}/shmtgt.wap)
set(WASM_MENU_BAR_SRC ${EXAMPLES_C_DIR}/menu_bar/menu_bar.c)
set(WASM_MENU_BAR_WASM ${BUILD_DIR}/menu_bar.wasm)
set(WASM_MENU_BAR_APP ${BUILD_DIR}/menu_bar.wap)
set(WASM_NET_SMOKE_SRC ${EXAMPLES_C_DIR}/net_smoke/net_smoke.c)
set(WASM_NET_SMOKE_WASM ${BUILD_DIR}/net_smoke.wasm)
set(WASM_NET_SMOKE_APP ${BUILD_DIR}/net_smoke.wap)
set(WASM_NET_UDP_ECHO_SRC ${EXAMPLES_C_DIR}/net_udp_echo/net_udp_echo.c)
set(WASM_NET_UDP_ECHO_WASM ${BUILD_DIR}/net_udp_echo.wasm)
set(WASM_NET_UDP_ECHO_APP ${BUILD_DIR}/net_udp_echo.wap)
set(WASM_NET_TCP_ECHO_SRC ${EXAMPLES_C_DIR}/net_tcp_echo/net_tcp_echo.c)
set(WASM_NET_TCP_ECHO_WASM ${BUILD_DIR}/net_tcp_echo.wasm)
set(WASM_NET_TCP_ECHO_APP ${BUILD_DIR}/net_tcp_echo.wap)
set(WASM_NET_TCP_SERVER_SRC ${EXAMPLES_C_DIR}/net_tcp_server/net_tcp_server.c)
set(WASM_NET_TCP_SERVER_WASM ${BUILD_DIR}/net_tcp_server.wasm)
set(WASM_NET_TCP_SERVER_APP ${BUILD_DIR}/net_tcp_server.wap)
set(WASM_ATA_DRIVER_SRC ${DRIVER_WASM_DIR}/ata/ata.c)
set(WASM_ATA_DRIVER_WASM ${BUILD_DIR}/ata.wasm)
set(WASM_ATA_APP ${BUILD_DIR}/ata.wap)
set(WASM_FAT_DRIVER_SRC
  ${DRIVER_WASM_DIR}/fs_fat/fs_fat.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_util.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_block.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_geom.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_name.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_alloc.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_dir.c
  ${DRIVER_WASM_DIR}/fs_fat/fat_file.c)
set(WASM_FAT_DRIVER_WASM ${BUILD_DIR}/fs_fat.wasm)
set(WASM_FAT_APP ${BUILD_DIR}/fs_fat.wap)
set(WASM_WFS_DRIVER_SRC
  ${DRIVER_WASM_DIR}/fs_wfs/fs_wfs.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_mount.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_extent.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_dir.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_path.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_read.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_fd.c)
set(WASM_WFS_DRIVER_WASM ${BUILD_DIR}/fs_wfs.wasm)
set(WASM_WFS_APP ${BUILD_DIR}/fs_wfs.wap)
set(WASM_INITFS_DRIVER_SRC ${DRIVER_WASM_DIR}/fs_init/fs_init.c)
set(WASM_INITFS_DRIVER_WASM ${BUILD_DIR}/fs_init.wasm)
set(WASM_INITFS_APP ${BUILD_DIR}/fs_init.wap)
set(WASM_SERIAL_DRIVER_SRC ${DRIVER_WASM_DIR}/serial/serial.ts)
set(WASM_SERIAL_DRIVER_WASM ${BUILD_DIR}/serial.wasm)
set(WASM_SERIAL_APP ${BUILD_DIR}/serial.wap)
set(WASM_KEYBOARD_DRIVER_SRC ${DRIVER_WASM_DIR}/keyboard/keyboard.ts)
set(WASM_KEYBOARD_DRIVER_WASM ${BUILD_DIR}/keyboard.wasm)
set(WASM_KEYBOARD_APP ${BUILD_DIR}/keyboard.wap)
set(WASM_MOUSE_DRIVER_SRC ${DRIVER_WASM_DIR}/mouse/mouse.ts)
set(WASM_MOUSE_DRIVER_WASM ${BUILD_DIR}/mouse.wasm)
set(WASM_MOUSE_APP ${BUILD_DIR}/mouse.wap)
set(WASM_RTC_DRIVER_SRC ${DRIVER_WASM_DIR}/rtc/rtc.ts)
set(WASM_RTC_DRIVER_WASM ${BUILD_DIR}/rtc.wasm)
set(WASM_RTC_APP ${BUILD_DIR}/rtc.wap)
set(WASM_VIRTIO_NET_DRIVER_SRC ${DRIVER_WASM_DIR}/virtio_net/virtio_net.c)
set(WASM_VIRTIO_NET_DRIVER_WASM ${BUILD_DIR}/virtio_net.wasm)
set(WASM_VIRTIO_NET_APP ${BUILD_DIR}/virtio_net.wap)
set(WASM_NET_STACK_APP ${BUILD_DIR}/net_stack.wap)
set(WASM_VIRTIO_SERIAL_DRIVER_SRC ${DRIVER_WASM_DIR}/virtio_serial/virtio_serial.c)
set(WASM_VIRTIO_SERIAL_DRIVER_WASM ${BUILD_DIR}/virtio_serial.wasm)
set(WASM_VIRTIO_SERIAL_APP ${BUILD_DIR}/virtio_serial.wap)
set(WASM_VIRTIO_RNG_DRIVER_SRC ${DRIVER_WASM_DIR}/virtio_rng/virtio_rng.c)
set(WASM_VIRTIO_RNG_DRIVER_WASM ${BUILD_DIR}/virtio_rng.wasm)
set(WASM_VIRTIO_RNG_APP ${BUILD_DIR}/virtio_rng.wap)
set(WASM_FRAMEBUFFER_APP ${BUILD_DIR}/framebuffer.wap)
set(NATIVE_FRAMEBUFFER_DRIVER_SRC ${DRIVER_WASM_DIR}/framebuffer/framebuffer_native.c)
set(NATIVE_FRAMEBUFFER_RENDER_SRC ${DRIVER_WASM_DIR}/framebuffer/render.c)
set(NATIVE_FRAMEBUFFER_DRIVER_ELF ${BUILD_DIR}/framebuffer_native.elf)
set(NATIVE_FRAMEBUFFER_DRIVER_OBJ ${BUILD_DIR}/framebuffer_native.o)
set(NATIVE_FRAMEBUFFER_RENDER_OBJ ${BUILD_DIR}/framebuffer_render.o)
set(WASM_FRAMEBUFFER_PCI_APP ${BUILD_DIR}/framebuffer_pci.wap)
set(NATIVE_FRAMEBUFFER_PCI_DRIVER_SRC ${DRIVER_WASM_DIR}/framebuffer_pci/framebuffer_pci_native.c)
set(NATIVE_FRAMEBUFFER_PCI_DRIVER_ELF ${BUILD_DIR}/framebuffer_pci_native.elf)
set(WASM_INIT_APP_SRC ${SERVICES_DIR}/sysinit/init.c)
set(WASM_INIT_APP_WASM ${BUILD_DIR}/init.wasm)
set(WASM_INIT_APP ${BUILD_DIR}/sysinit.wap)
set(WASM_CLI_APP_SRC ${SERVICES_DIR}/cli/cli.c ${SERVICES_DIR}/cli/cli_ls_order.c)
set(WASM_CLI_APP_WASM ${BUILD_DIR}/cli.wasm)
set(WASM_CLI_APP ${BUILD_DIR}/cli.wap)
set(WASM_DEVICE_MANAGER_SOURCES
  ${SERVICES_DIR}/device_manager/device_manager.c
  ${SERVICES_DIR}/device_manager/device_manager_rules.c
)
set(WASM_DEVICE_MANAGER_WASM ${BUILD_DIR}/device_manager.wasm)
set(WASM_DEVICE_MANAGER_APP ${BUILD_DIR}/device_manager.wap)
set(WASM_PCI_BUS_SRC ${SERVICES_DIR}/pci_bus/pci_bus.c)
set(WASM_PCI_BUS_WASM ${BUILD_DIR}/pci_bus.wasm)
set(WASM_PCI_BUS_APP ${BUILD_DIR}/pci_bus.wap)
set(WASM_ACPI_BUS_SRC ${SERVICES_DIR}/acpi_bus/acpi_bus.c)
set(WASM_ACPI_BUS_WASM ${BUILD_DIR}/acpi_bus.wasm)
set(WASM_ACPI_BUS_APP ${BUILD_DIR}/acpi_bus.wap)
set(WASM_FS_MANAGER_SRC ${SERVICES_DIR}/fs_manager/fs_manager.c)
set(WASM_FS_MANAGER_WASM ${BUILD_DIR}/fs_manager.wasm)
set(WASM_FS_MANAGER_APP ${BUILD_DIR}/fs_manager.wap)
set(WASM_VT_APP_SRC ${SERVICES_DIR}/vt/vt_main.c)
set(WASM_VT_APP_WASM ${BUILD_DIR}/vt.wasm)
set(WASM_VT_APP ${BUILD_DIR}/vt.wap)
set(WASM_GFX_COMPOSITOR_SRC ${SERVICES_DIR}/gfx_compositor/gfx_compositor.zig)
set(WASM_GFX_COMPOSITOR_WASM ${BUILD_DIR}/gfx_compositor.elf)
set(WASM_GFX_COMPOSITOR_APP ${BUILD_DIR}/gfx_compositor.wap)
set(WASM_FONT_SERVICE_SRC ${SERVICES_DIR}/font_service/font_service.zig)
set(WASM_FONT_SERVICE_WASM ${BUILD_DIR}/font_service.elf)
set(WASM_FONT_SERVICE_APP ${BUILD_DIR}/font_service.wap)
set(WASM_HELLO_C_SRC ${EXAMPLES_C_DIR}/hello/hello_c.c)
set(WASM_HELLO_C_WASM ${BUILD_DIR}/hello_c.wasm)
set(WASM_HELLO_C_APP ${BUILD_DIR}/hello_c.wap)
set(WASM_HELLO_RUST_SRC ${EXAMPLES_RUST_DIR}/hello/hello_rust.rs)
set(WASM_HELLO_RUST_WASM ${BUILD_DIR}/hello_rust.wasm)
set(WASM_HELLO_RUST_APP ${BUILD_DIR}/hello_rust.wap)
set(WASM_TETRIS_RUST_SRC ${EXAMPLES_RUST_DIR}/tetris/tetris.rs)
set(WASM_TETRIS_RUST_WASM ${BUILD_DIR}/tetris.wasm)
set(WASM_TETRIS_RUST_APP ${BUILD_DIR}/tetris.wap)
set(WASM_HELLO_GO_SRC ${EXAMPLES_GO_DIR}/hello/hello_go.go)
set(WASM_HELLO_GO_WASM ${BUILD_DIR}/hello_go.wasm)
set(WASM_HELLO_GO_APP ${BUILD_DIR}/hello_go.wap)
set(WASM_HELLO_ZIG_SRC ${EXAMPLES_ZIG_DIR}/hello/hello_zig.zig)
set(WASM_HELLO_ZIG_WASM ${BUILD_DIR}/hello_zig.wasm)
set(WASM_HELLO_ZIG_APP ${BUILD_DIR}/hello_zig.wap)
set(WASM_CALCULATOR_SRC ${EXAMPLES_ZIG_DIR}/calculator/calculator.zig)
set(WASM_CALCULATOR_WASM ${BUILD_DIR}/calculator.wasm)
set(WASM_CALCULATOR_APP ${BUILD_DIR}/calculator.wap)
set(AS_HELLO_SRC ${EXAMPLES_AS_DIR}/hello/app.ts)
set(AS_HELLO_WASM ${BUILD_DIR}/hello_as.wasm)
set(AS_HELLO_APP ${BUILD_DIR}/hello_as.wap)
set(AS_MINESWEEPER_SRC ${EXAMPLES_AS_DIR}/minesweeper/app.ts)
set(AS_MINESWEEPER_WASM ${BUILD_DIR}/minesweeper.wasm)
set(AS_MINESWEEPER_APP ${BUILD_DIR}/minesweeper.wap)
set(WASM_DATE_TOOL_SRC ${CMAKE_SOURCE_DIR}/src/utils/date/date.ts)
set(WASM_DATE_TOOL_WASM ${BUILD_DIR}/date_tool.wasm)
set(WASM_DATE_TOOL_APP ${BUILD_DIR}/date.wap)
set(WASM_SCHED_INFO_SRC ${CMAKE_SOURCE_DIR}/src/utils/sched_info/sched_info.ts)
set(WASM_SCHED_INFO_WASM ${BUILD_DIR}/sched_info.wasm)
set(WASM_SCHED_INFO_APP ${BUILD_DIR}/sched_info.wap)
set(WASM_PS_SRC ${CMAKE_SOURCE_DIR}/src/utils/ps/ps.zig)
set(WASM_PS_WASM ${BUILD_DIR}/ps.wasm)
set(WASM_PS_APP ${BUILD_DIR}/ps.wap)
set(WASM_CAT_SRC ${CMAKE_SOURCE_DIR}/src/utils/cat/cat.zig)
set(WASM_CAT_WASM ${BUILD_DIR}/cat.wasm)
set(WASM_CAT_APP ${BUILD_DIR}/cat.wap)
set(WASM_SYSINFO_SRC ${CMAKE_SOURCE_DIR}/src/utils/sysinfo/sysinfo.zig)
set(WASM_SYSINFO_WASM ${BUILD_DIR}/sysinfo.wasm)
set(WASM_SYSINFO_APP ${BUILD_DIR}/sysinfo.wap)
set(WASM_EXPLORER_SRC ${CMAKE_SOURCE_DIR}/src/utils/explorer/explorer.c)
set(WASM_EXPLORER_WASM ${BUILD_DIR}/explorer.wasm)
set(WASM_EXPLORER_APP ${BUILD_DIR}/explorer.wap)
set(WASM_IP_SRC ${CMAKE_SOURCE_DIR}/src/utils/ip/ip.c)
set(WASM_IP_WASM ${BUILD_DIR}/ip.wasm)
set(WASM_IP_APP ${BUILD_DIR}/ip.wap)
set(WASM_HOST_SRC ${CMAKE_SOURCE_DIR}/src/utils/host/host.c)
set(WASM_HOST_WASM ${BUILD_DIR}/host.wasm)
set(WASM_HOST_APP ${BUILD_DIR}/host.wap)
set(WASM_CURL_SRC ${CMAKE_SOURCE_DIR}/src/utils/curl/curl.c)
set(WASM_CURL_WASM ${BUILD_DIR}/curl.wasm)
set(WASM_CURL_APP ${BUILD_DIR}/curl.wap)

if (NOT DEFINED AS_ENABLE_INITIALIZED)
  set(AS_ENABLE ON CACHE BOOL "Enable AssemblyScript example builds" FORCE)
  set(AS_ENABLE_INITIALIZED ON CACHE BOOL "Set when AS_ENABLE default is initialized" FORCE)
endif ()
option(AS_ENABLE "Enable AssemblyScript example builds" ON)

if (NOT DEFINED RUST_ENABLE_INITIALIZED)
  set(RUST_ENABLE ON CACHE BOOL "Enable Rust example builds" FORCE)
  set(RUST_ENABLE_INITIALIZED ON CACHE BOOL "Set when RUST_ENABLE default is initialized" FORCE)
endif ()
option(RUST_ENABLE "Enable Rust example builds" ON)

if (NOT DEFINED GO_ENABLE_INITIALIZED)
  set(GO_ENABLE ON CACHE BOOL "Enable Go (TinyGo) example builds" FORCE)
  set(GO_ENABLE_INITIALIZED ON CACHE BOOL "Set when GO_ENABLE default is initialized" FORCE)
endif ()
option(GO_ENABLE "Enable Go (TinyGo) example builds" ON)
set(GO_AVAILABLE OFF)
if (NOT DEFINED ZIG_ENABLE_INITIALIZED)
  set(ZIG_ENABLE ON CACHE BOOL "Enable Zig example builds" FORCE)
  set(ZIG_ENABLE_INITIALIZED ON CACHE BOOL "Set when ZIG_ENABLE default is initialized" FORCE)
endif ()
option(ZIG_ENABLE "Enable Zig example builds" ON)
set(ZIG_AVAILABLE OFF)
option(WASMOS_TRACE "Enable verbose kernel and userland tracing" OFF)
if (WASMOS_TRACE)
  set(WASMOS_TRACE_VALUE 1)
else ()
  set(WASMOS_TRACE_VALUE 0)
endif ()
set(WASMOS_TRACE_DEFINE -DWASMOS_TRACE=${WASMOS_TRACE_VALUE})

# Runtime loading of bundled AOT binaries.  OFF forces every WARP app down the
# JIT path (initFromBytecode) without changing any packed artifact, so an AOT
# build and a JIT build differ only in the kernel.
option(WASMOS_WARP_RUNTIME_LOAD_AOT "Load bundled AOT binaries at runtime" ON)
if (WASMOS_WARP_RUNTIME_LOAD_AOT)
  set(WASMOS_WARP_RUNTIME_LOAD_AOT_VALUE 1)
else ()
  set(WASMOS_WARP_RUNTIME_LOAD_AOT_VALUE 0)
endif ()
set(WASMOS_WARP_RUNTIME_LOAD_AOT_DEFINE
    -DWASMOS_WARP_RUNTIME_LOAD_AOT=${WASMOS_WARP_RUNTIME_LOAD_AOT_VALUE})

function(wasmos_add_ide_c_target target_name)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES INCLUDES IQUOTE DEFINES)
  cmake_parse_arguments(WASMOS_IDE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if (NOT WASMOS_IDE_SOURCES)
    message(FATAL_ERROR "wasmos_add_ide_c_target(${target_name}) requires SOURCES")
  endif ()

  add_library(${target_name} OBJECT EXCLUDE_FROM_ALL ${WASMOS_IDE_SOURCES})
  if (WASMOS_IDE_INCLUDES)
    target_include_directories(${target_name} PRIVATE ${WASMOS_IDE_INCLUDES})
  endif ()
  # IQUOTE dirs serve "quoted" includes only, leaving <angle> ones to the host
  # headers. target_include_directories cannot emit -iquote, hence the raw flag.
  # SHELL: keeps each flag/value pair together; without it CMake de-duplicates
  # the repeated -iquote token and the paths collapse onto the first one.
  foreach(_q ${WASMOS_IDE_IQUOTE})
    target_compile_options(${target_name} PRIVATE "SHELL:-iquote ${_q}")
  endforeach()
  # Some sources only parse under the same -D the real compile line passes (a
  # header may select between an inline asm body and a portable one), so the
  # index has to carry them or clang-tidy analyses a variant nothing builds.
  if (WASMOS_IDE_DEFINES)
    target_compile_definitions(${target_name} PRIVATE ${WASMOS_IDE_DEFINES})
  endif ()
  # Force-include the cast macros so compile_commands.json entries (used by
  # clang-tidy) resolve ptr_cast/addr_cast/fn_cast, matching the real build.
  target_compile_options(${target_name} PRIVATE -include ${LIBC_DIR}/include/wasmos_cast.h)
  target_compile_definitions(${target_name} PRIVATE
    WASMOS_TRACE=${WASMOS_TRACE_VALUE}
    WASMOS_IRQ_MODE=${WASMOS_IRQ_MODE_VALUE}
    WASMOS_SMP=${WASMOS_SMP_VALUE})
endfunction()
option(WASMOS_RING3_SMOKE "Enable ring3 smoke probe spawn by default" OFF)
if (WASMOS_RING3_SMOKE)
  set(WASMOS_RING3_SMOKE_VALUE 1)
else ()
  set(WASMOS_RING3_SMOKE_VALUE 0)
endif ()
set(WASMOS_RING3_SMOKE_DEFINE -DWASMOS_RING3_SMOKE_DEFAULT=${WASMOS_RING3_SMOKE_VALUE})

# Standalone SMP scheduler stress test (separate from the always-on selftests).
# When ON, the kernel spawns a cross-CPU token-ring stress + invariant test
# before entering the scheduler loop; see kernel_sched_smp_stress_runtime.c.
option(WASMOS_SCHED_SMP_STRESS "Run the standalone SMP scheduler stress test" OFF)
if (WASMOS_SCHED_SMP_STRESS)
  set(WASMOS_SCHED_SMP_STRESS_DEFINE -DWASMOS_SCHED_SMP_STRESS=1)
else ()
  set(WASMOS_SCHED_SMP_STRESS_DEFINE)
endif ()
option(WASMOS_RING3_THREAD_LIFECYCLE_SMOKE
       "Enable dedicated ring3 thread lifecycle helper probe spawn by default"
       OFF)
if (WASMOS_RING3_THREAD_LIFECYCLE_SMOKE)
  set(WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_VALUE 1)
else ()
  set(WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_VALUE 0)
endif ()
set(WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFINE
    -DWASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT=${WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_VALUE})
option(WASMOS_PM_TEST_HOOKS "Enable process-manager test inject hooks" OFF)
if (WASMOS_PM_TEST_HOOKS)
  set(WASMOS_PM_TEST_HOOKS_VALUE 1)
else ()
  set(WASMOS_PM_TEST_HOOKS_VALUE 0)
endif ()
set(WASMOS_PM_TEST_HOOKS_DEFINE -DWASMOS_PM_TEST_HOOKS=${WASMOS_PM_TEST_HOOKS_VALUE})

if (NOT DEFINED WASMOS_PM_LIST_LINKED_INITIALIZED)
  set(WASMOS_PM_LIST_LINKED ON CACHE BOOL "Use linked-list backend for process-manager dynamic lists" FORCE)
  set(WASMOS_PM_LIST_LINKED_INITIALIZED ON CACHE BOOL "Set when WASMOS_PM_LIST_LINKED default is initialized" FORCE)
endif ()
option(WASMOS_PM_LIST_LINKED "Use linked-list backend for process-manager dynamic lists" ON)

if (NOT DEFINED WASMOS_PM_LIST_ARRAY_CHUNK_INITIALIZED)
  set(WASMOS_PM_LIST_ARRAY_CHUNK OFF CACHE BOOL "Use array-chunk backend for process-manager dynamic lists" FORCE)
  set(WASMOS_PM_LIST_ARRAY_CHUNK_INITIALIZED ON CACHE BOOL "Set when WASMOS_PM_LIST_ARRAY_CHUNK default is initialized" FORCE)
endif ()
option(WASMOS_PM_LIST_ARRAY_CHUNK "Use array-chunk backend for process-manager dynamic lists" OFF)

if (NOT DEFINED WASMOS_PM_LIST_ARRAY_CHUNK_CAP_INITIALIZED)
  set(WASMOS_PM_LIST_ARRAY_CHUNK_CAP 16 CACHE STRING "Array-chunk capacity for process-manager list backend" FORCE)
  set(WASMOS_PM_LIST_ARRAY_CHUNK_CAP_INITIALIZED ON CACHE BOOL "Set when WASMOS_PM_LIST_ARRAY_CHUNK_CAP default is initialized" FORCE)
endif ()
set(WASMOS_PM_LIST_ARRAY_CHUNK_CAP "${WASMOS_PM_LIST_ARRAY_CHUNK_CAP}" CACHE STRING "Array-chunk capacity for process-manager list backend")

if (WASMOS_PM_LIST_ARRAY_CHUNK)
  set(WASMOS_PM_LIST_IMPL_VALUE 1)
else ()
  set(WASMOS_PM_LIST_IMPL_VALUE 0)
endif ()
set(WASMOS_PM_LIST_DEFINE -DWASMOS_PM_LIST_IMPL=${WASMOS_PM_LIST_IMPL_VALUE})
set(WASMOS_PM_LIST_CAP_DEFINE -DWASMOS_PM_LIST_ARRAY_CHUNK_CAP=${WASMOS_PM_LIST_ARRAY_CHUNK_CAP})

# Default runtime: WARP. A guest under WARP runs at CPL=3 and is preempted like
# any other thread; the wasm3 interpreter and the guest it interprets both run
# at CPL=0, where process_preempt_from_irq refuses to preempt at all, so a guest
# loop with no host call in it holds its CPU until it returns (see
# architecture/11, "Which Workloads Reach Ring 3"). That makes wasm3 a poor
# default for an interactive system -- one heavy app stalls the desktop -- while
# remaining valuable as the reference implementation the two runtimes are read
# against. Pin either explicitly with
# -DWASMOS_DOTCONFIG=configs/{wasm3,warp}_{single,smp}_defconfig.
if (NOT DEFINED WASMOS_WASM_RUNTIME_DEFAULT_INITIALIZED)
  set(WASMOS_WASM_RUNTIME_WARP  ON  CACHE BOOL "Use WARP JIT compiler as WASM runtime" FORCE)
  set(WASMOS_WASM_RUNTIME_WASM3 OFF CACHE BOOL "Use wasm3 interpreter as WASM runtime" FORCE)
  set(WASMOS_WASM_RUNTIME_DEFAULT_INITIALIZED ON CACHE BOOL "Set when WASMOS_WASM_RUNTIME default is initialized" FORCE)
endif ()
option(WASMOS_WASM_RUNTIME_WASM3 "Use wasm3 interpreter as WASM runtime" OFF)
option(WASMOS_WASM_RUNTIME_WARP  "Use WARP JIT compiler as WASM runtime" ON)

# WASM_RUNTIME_INCLUDE_DIR and WASM_RUNTIME_DEFINE are computed after the
# Kconfig import (below) so that on a fresh cmake cache the kconfig-selected
# runtime overrides the initialization-block default before CFLAGS_KERNEL
# is assembled.  Placeholder values keep the variables defined for any code
# between here and the kconfig import that might inspect them (none currently).
set(WASM_RUNTIME_INCLUDE_DIR ${WASM3_DIR})
set(WASM_RUNTIME_DEFINE -DWASMOS_WASM_RUNTIME=0)

if (NOT DEFINED WASMOS_IRQ_PIC_INITIALIZED)
  set(WASMOS_IRQ_PIC ON CACHE BOOL "Use legacy 8259 PIC as IRQ controller" FORCE)
  set(WASMOS_IRQ_PIC_INITIALIZED ON CACHE BOOL "Set when WASMOS_IRQ_PIC default is initialized" FORCE)
endif ()
option(WASMOS_IRQ_PIC "Use legacy 8259 PIC as IRQ controller" ON)

if (NOT DEFINED WASMOS_IRQ_LAPIC_INITIALIZED)
  set(WASMOS_IRQ_LAPIC OFF CACHE BOOL "Use LAPIC only (LAPIC timer, no external device IRQs)" FORCE)
  set(WASMOS_IRQ_LAPIC_INITIALIZED ON CACHE BOOL "Set when WASMOS_IRQ_LAPIC default is initialized" FORCE)
endif ()
option(WASMOS_IRQ_LAPIC "Use LAPIC only (LAPIC timer, no external device IRQs)" OFF)

if (NOT DEFINED WASMOS_IRQ_IOAPIC_INITIALIZED)
  set(WASMOS_IRQ_IOAPIC OFF CACHE BOOL "Use LAPIC + I/O APIC (full APIC)" FORCE)
  set(WASMOS_IRQ_IOAPIC_INITIALIZED ON CACHE BOOL "Set when WASMOS_IRQ_IOAPIC default is initialized" FORCE)
endif ()
option(WASMOS_IRQ_IOAPIC "Use LAPIC + I/O APIC (full APIC)" OFF)

if (NOT DEFINED WASMOS_SMP_INITIALIZED)
  set(WASMOS_SMP OFF CACHE BOOL "Enable Symmetric Multi-Processing (SMP)" FORCE)
  set(WASMOS_SMP_INITIALIZED ON CACHE BOOL "Set when WASMOS_SMP default is initialized" FORCE)
endif ()
option(WASMOS_SMP "Enable Symmetric Multi-Processing (SMP)" OFF)

# IRQ mode determination is deferred — see below, after the Kconfig import.

set(CLANG clang CACHE STRING "C compiler for freestanding/UEFI builds")
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(LLD ld.lld CACHE STRING "Linker (lld or ld.lld)")
else ()
  set(LLD lld CACHE STRING "Linker (lld or ld.lld)")
endif ()
set(LLD_LINK lld-link CACHE STRING "COFF linker (lld-link) for UEFI on macOS")
set(OBJCOPY llvm-objcopy CACHE STRING "Objcopy tool")
set(OVMF_CODE "" CACHE FILEPATH "Path to OVMF_CODE.fd")
set(OVMF_VARS "" CACHE FILEPATH "Path to OVMF_VARS.fd (optional)")
set(KERNEL_TARGET_TRIPLE x86_64-elf CACHE STRING "Target triple for kernel build")

set(LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /opt/homebrew/bin /usr/local/bin)

find_program(CLANG_PATH ${CLANG} HINTS ${LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CLANG_PATH)
  find_program(CLANG_PATH ${CLANG})
endif ()
if (NOT CLANG_PATH)
  message(FATAL_ERROR "CLANG not found. Install LLVM clang and set -DCLANG=/path/to/llvm/bin/clang if needed.")
endif ()
set(CLANG ${CLANG_PATH} CACHE STRING "C compiler for freestanding/UEFI builds" FORCE)
set(CMAKE_C_COMPILER ${CLANG} CACHE FILEPATH "Use LLVM clang for all C builds" FORCE)
set(CMAKE_ASM_COMPILER ${CLANG} CACHE FILEPATH "Use LLVM clang for all ASM builds" FORCE)
get_filename_component(CLANG_BIN_DIR ${CLANG} DIRECTORY)
find_program(CLANGXX_PATH clang++ HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CLANGXX_PATH)
  find_program(CLANGXX_PATH clang++)
endif ()
if (NOT CLANGXX_PATH)
  message(FATAL_ERROR "clang++ not found. Install LLVM clang++ for C++ WASM modules.")
endif ()
set(CMAKE_CXX_COMPILER ${CLANGXX_PATH} CACHE FILEPATH "Use LLVM clang++ for all C++ builds" FORCE)

project(wasmos C CXX ASM)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include(CMakeParseArguments)

# Sources every WASM C target compiles for itself, on top of the sysroot
# archives. Only ENTRY shims belong here: an entry symbol must be present
# unconditionally, and an archive member is pulled only when something already
# references it. Everything else -- libc and libsys -- comes from
# ${WASMOS_SDK_LIBDIR}/lib{c,sys}.a, built once by cmake/WasmosSdk.cmake with the
# same flags this helper passes, so there is one libc build rather than one per
# module and one link line rather than two to keep in step.
set(WASMOS_LIBC_C_SOURCES)

set(WASMOS_LIBC_INCLUDE_DEPS
  ${LIBC_DIR}/include/ctype.h
  ${LIBC_DIR}/include/stdio.h
  ${LIBC_DIR}/include/stdlib.h
  ${LIBC_DIR}/include/string.h
  ${LIBC_DIR}/include/wasmos/api.h
  ${LIBC_DIR}/include/wasmos/ipc.h
  ${LIBC_DIR}/include/wasmos/imports.h
  ${LIBC_DIR}/include/wasmos/script.h
  ${LIBSYS_WASM_DIR}/include/wasmos/libsys.h
  ${LIBSYS_WASM_DIR}/include/wasmos/coroutine_wasm.h
  ${LIBSYS_WASM_DIR}/include/wasmos/libsys_string.h
  ${LIBSYS_WASM_DIR}/include/wasmos/sha256.h
  ${SERVICES_DIR}/fs_manager/fs_manager_types.h
  ${SERVICES_DIR}/device_manager/device_manager_types.h
  ${SERVICES_DIR}/pci_bus/pci_bus_types.h
  ${SERVICES_DIR}/sysinit/sysinit_types.h
  ${SERVICES_DIR}/vt/vt_types.h
  ${SERVICES_DIR}/cli/cli_types.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_label.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_button.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_checkbox.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_text_input.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_list_view.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_dropdown.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_scroll_view.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_menu_bar.h
  ${CMAKE_SOURCE_DIR}/src/libui/include/wasmos/libui_menu_item.h
)

# wasmos_manifest_link_value(<out_var> <manifest> <key> <default>)
#
# Reads one key from a manifest's [link] section -- the link-time memory sizes,
# in bytes -- and falls back to <default> when the section or the key is absent.
#
# Section-aware rather than a bare regex over the file: [resources] holds
# stack_pages next to [link]'s stack_size, and a manifest is the one place a
# reader looks to learn how an app is sized, so a key silently read from the wrong
# section would be worse than no key at all. The grammar is the subset
# scripts/make_wasmos_app.c accepts (see parse_linker_manifest); [link] is the one
# section the packer ignores, because these values reach the linker, not the
# container.
function(wasmos_manifest_link_value out_var manifest key default)
  set(_result ${default})
  if (EXISTS "${manifest}")
    file(STRINGS "${manifest}" _lines)
    set(_section "")
    foreach (_line IN LISTS _lines)
      string(REGEX REPLACE "#.*$" "" _line "${_line}")
      string(STRIP "${_line}" _line)
      if (_line STREQUAL "")
        continue()
      endif ()
      if (_line MATCHES "^\\[")
        set(_section "${_line}")
        continue()
      endif ()
      if (_section STREQUAL "[link]" AND _line MATCHES "^${key}[ \t]*=[ \t]*(.+)$")
        string(STRIP "${CMAKE_MATCH_1}" _value)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" _value "${_value}")
        set(_result "${_value}")
      endif ()
    endforeach ()
  endif ()
  set(${out_var} ${_result} PARENT_SCOPE)
endfunction()

# wasmos_manifest_link_pages(<out_var> <manifest> <key> <default_pages>)
#
# The same [link] value as wasmos_manifest_link_value, converted to WASM pages for
# the toolchains that take a page count (asc's --initialMemory). The manifest is in
# bytes whatever the language, so an app reads the same way in every one of them;
# a value that is not a whole number of 64 KiB pages is an error rather than a
# silent round, because rounding down would hand the app less memory than it asked
# for and rounding up would hide a typo.
function(wasmos_manifest_link_pages out_var manifest key default_pages)
  wasmos_manifest_link_value(_bytes "${manifest}" ${key} 0)
  if (_bytes EQUAL 0)
    set(${out_var} ${default_pages} PARENT_SCOPE)
    return()
  endif ()
  math(EXPR _pages "${_bytes} / 65536")
  math(EXPR _remainder "${_bytes} % 65536")
  if (NOT _remainder EQUAL 0)
    message(FATAL_ERROR
      "${manifest}: [link] ${key} = ${_bytes} is not a whole number of 64 KiB WASM "
      "pages. Round it to a multiple of 65536.")
  endif ()
  set(${out_var} ${_pages} PARENT_SCOPE)
endfunction()

function(wasmos_add_wasm_c_app_target target_name)
  set(options NO_BUILTIN STARTUP_SHIM CXX ASYNC_SERVICE ASYNC_APP)
  set(oneValueArgs SOURCE OUTPUT_WASM OUTPUT_APP MANIFEST EXPORT STACK_SIZE INITIAL_MEMORY MAX_MEMORY BUILD_COMMENT PACK_COMMENT)
  # EXPORT, STARTUP_SHIM and NO_BUILTIN are still parsed so that passing one is a
  # loud error rather than a silent no-op; see the checks below.
  set(multiValueArgs SOURCES)
  cmake_parse_arguments(WASMOS_APP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(_wasmos_sources ${WASMOS_APP_SOURCES})
  if (NOT _wasmos_sources AND WASMOS_APP_SOURCE)
    set(_wasmos_sources ${WASMOS_APP_SOURCE})
  endif ()

  if (NOT _wasmos_sources OR NOT WASMOS_APP_OUTPUT_WASM OR NOT WASMOS_APP_OUTPUT_APP OR
      NOT WASMOS_APP_MANIFEST)
    message(FATAL_ERROR "wasmos_add_wasm_c_app_target(${target_name}) requires SOURCE or SOURCES, OUTPUT_WASM, OUTPUT_APP, MANIFEST")
  endif ()
  # The entry export, whether a crt is linked, and -fno-builtin are all decided by
  # the manifest and the driver now, so naming them here would be a second source
  # for a value that already has one.
  if (WASMOS_APP_EXPORT)
    message(FATAL_ERROR
      "${target_name}: EXPORT is read from the manifest's [package] entry now. "
      "Remove it; ${WASMOS_APP_MANIFEST} already carries it.")
  endif ()
  if (WASMOS_APP_STARTUP_SHIM)
    message(FATAL_ERROR
      "${target_name}: STARTUP_SHIM is implied by the manifest's entry -- crt1.o is "
      "linked exactly when that entry is wasmos_main. Remove it.")
  endif ()
  if (WASMOS_APP_NO_BUILTIN)
    message(FATAL_ERROR
      "${target_name}: NO_BUILTIN is unconditional now (the driver always passes "
      "-fno-builtin, since libc owns the mem* family). Remove it.")
  endif ()

  # The SDK driver is the compiler: it owns the triple, the sysroot, the
  # freestanding/nostdlib flags, the wasm linker defaults, crt1 and the runtime
  # libraries. Every one of those used to be spelled out here as well, which meant
  # two link lines describing one toolchain -- and only one of them was exercised by
  # the tree. wasmos-clang++ is the same driver with the C++ flag set.
  set(_wasmos_driver ${WASMOS_SDK_DIR}/bin/wasmos-clang)
  set(_wasmos_driver_flags)
  if (WASMOS_APP_CXX)
    list(APPEND _wasmos_driver_flags --wasmos-cxx)
  endif ()

  set(_wasmos_libc_sources ${WASMOS_LIBC_C_SOURCES})
  if (WASMOS_APP_ASYNC_SERVICE AND WASMOS_APP_ASYNC_APP)
    message(FATAL_ERROR "async WASM target may be a service/driver or an app, not both")
  endif ()
  if (WASMOS_APP_ASYNC_SERVICE)
    list(APPEND _wasmos_libc_sources ${LIBSYS_WASM_DIR}/service_async_entry_wasm.c)
  elseif (WASMOS_APP_ASYNC_APP)
    list(APPEND _wasmos_libc_sources ${LIBSYS_WASM_DIR}/app_async_entry_wasm.c)
  endif ()

  # Depended on, not named: the driver links these out of the sysroot itself.
  # These are the archives' SOURCES rather than the archives, because this
  # function is called from subdirectories and a subdirectory cannot depend on
  # another directory's custom-command output (see WASMOS_SDK_ARCHIVE_SOURCES in
  # cmake/WasmosSdk.cmake). Ordering comes from the wasmos-sdk target dependency
  # added with the target below.
  set(_wasmos_runtime_deps ${WASMOS_SDK_ARCHIVE_SOURCES})

  # Link-time memory lives in the manifest's [link] section and is read by the
  # driver at build time, not here. These values are load-bearing rather than
  # stylistic -- an app that maps shared memory, a framebuffer or socket rings has
  # to reserve linear memory up front, because those windows are placed above a
  # 2 MiB floor and above live data -- so they belong with the app.
  if (WASMOS_APP_STACK_SIZE OR WASMOS_APP_INITIAL_MEMORY OR WASMOS_APP_MAX_MEMORY)
    message(FATAL_ERROR
      "${target_name}: STACK_SIZE/INITIAL_MEMORY/MAX_MEMORY are read from the "
      "manifest's [link] section now. Move them to "
      "${WASMOS_APP_MANIFEST} as stack_size/initial_memory/max_memory.")
  endif ()

  set(_build_comment "Building wasm module")
  if (WASMOS_APP_BUILD_COMMENT)
    set(_build_comment ${WASMOS_APP_BUILD_COMMENT})
  endif ()
  set(_pack_comment "Packing wasm as WASMOS-APP")
  if (WASMOS_APP_PACK_COMMENT)
    set(_pack_comment ${WASMOS_APP_PACK_COMMENT})
  endif ()

  add_custom_command(
    OUTPUT ${WASMOS_APP_OUTPUT_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${_wasmos_driver}
            --emit-wasm
            --wasmos-manifest=${WASMOS_APP_MANIFEST}
            ${_wasmos_driver_flags}
            ${WASMOS_TRACE_DEFINE}
            ${_wasmos_libc_sources}
            ${_wasmos_sources}
            -o ${WASMOS_APP_OUTPUT_WASM}
    DEPENDS ${_wasmos_sources} ${_wasmos_libc_sources} ${WASMOS_LIBC_INCLUDE_DEPS}
            ${WASMOS_APP_MANIFEST} ${_wasmos_runtime_deps}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT ${_build_comment}
    VERBATIM
  )

  wasmos_maybe_aot_pack(
    "${WASMOS_APP_MANIFEST}"
    "${WASMOS_APP_OUTPUT_WASM}"
    "${WASMOS_APP_OUTPUT_APP}"
  )

  add_custom_target(${target_name}
    DEPENDS ${WASMOS_APP_OUTPUT_APP}
    SOURCES ${_wasmos_sources} ${WASMOS_APP_MANIFEST}
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASMOS_APP_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ${target_name})

  wasmos_add_ide_c_target(${target_name}_ide
    SOURCES ${_wasmos_sources}
    INCLUDES ${CMAKE_SOURCE_DIR}/src/libui/include ${LIBC_DIR}/include ${LIBSYS_WASM_DIR}/include ${DRIVER_WASM_DIR}/include
  )
endfunction()

function(wasmos_add_wasm_cpp_app_target target_name)
  wasmos_add_wasm_c_app_target(${target_name} CXX ${ARGN})
endfunction()

function(wasmos_add_native_c_app_target target_name)
  set(options)
  set(oneValueArgs OUTPUT_ELF OUTPUT_APP MANIFEST ENTRYPOINT IMAGE_BASE COMPILE_COMMENT LINK_COMMENT PACK_COMMENT)
  set(multiValueArgs SOURCES INCLUDES COMPILE_FLAGS HEADER_DEPS LINK_FLAGS)
  cmake_parse_arguments(WASMOS_NATIVE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if (NOT WASMOS_NATIVE_SOURCES OR NOT WASMOS_NATIVE_OUTPUT_ELF OR NOT WASMOS_NATIVE_OUTPUT_APP OR
      NOT WASMOS_NATIVE_MANIFEST OR NOT WASMOS_NATIVE_ENTRYPOINT)
    message(FATAL_ERROR "wasmos_add_native_c_app_target(${target_name}) requires SOURCES, OUTPUT_ELF, OUTPUT_APP, MANIFEST, ENTRYPOINT")
  endif ()

  set(_native_include_flags)
  foreach(_inc IN LISTS WASMOS_NATIVE_INCLUDES)
    list(APPEND _native_include_flags -I${_inc})
  endforeach()

  set(_native_obj_outputs)
  foreach(_src IN LISTS WASMOS_NATIVE_SOURCES)
    get_filename_component(_src_we ${_src} NAME_WE)
    set(_obj ${BUILD_DIR}/${target_name}_${_src_we}.o)
    list(APPEND _native_obj_outputs ${_obj})
    add_custom_command(
      OUTPUT ${_obj}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
      COMMAND ${CLANG} ${WASMOS_NATIVE_COMPILE_FLAGS} ${_native_include_flags}
              -include ${LIBC_DIR}/include/wasmos_cast.h
              ${_src}
              -o ${_obj}
      DEPENDS ${_src} ${WASMOS_NATIVE_HEADER_DEPS}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Compiling ${_src_we}.c"
      VERBATIM
    )
  endforeach()

  set(_image_base)
  if (WASMOS_NATIVE_IMAGE_BASE)
    set(_image_base --image-base=${WASMOS_NATIVE_IMAGE_BASE})
  endif ()

  set(_link_comment "Linking native driver")
  if (WASMOS_NATIVE_LINK_COMMENT)
    set(_link_comment ${WASMOS_NATIVE_LINK_COMMENT})
  endif ()

  add_custom_command(
    OUTPUT ${WASMOS_NATIVE_OUTPUT_ELF}
    COMMAND ${LLD_PATH}
            ${_image_base}
            -e ${WASMOS_NATIVE_ENTRYPOINT}
            -static
            ${WASMOS_NATIVE_LINK_FLAGS}
            ${_native_obj_outputs}
            -o ${WASMOS_NATIVE_OUTPUT_ELF}
    DEPENDS ${_native_obj_outputs}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT ${_link_comment}
    VERBATIM
  )

  set(_pack_comment "Packing native driver as WASMOS-APP")
  if (WASMOS_NATIVE_PACK_COMMENT)
    set(_pack_comment ${WASMOS_NATIVE_PACK_COMMENT})
  endif ()
  add_custom_command(
    OUTPUT ${WASMOS_NATIVE_OUTPUT_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${WASMOS_NATIVE_MANIFEST}
            --in ${WASMOS_NATIVE_OUTPUT_ELF}
            --out ${WASMOS_NATIVE_OUTPUT_APP}
    DEPENDS ${WASMOS_NATIVE_OUTPUT_ELF} make_wasmos_app ${WASMOS_NATIVE_MANIFEST}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT ${_pack_comment}
    VERBATIM
  )

  add_custom_target(${target_name}
    DEPENDS ${WASMOS_NATIVE_OUTPUT_APP}
    SOURCES ${WASMOS_NATIVE_SOURCES} ${WASMOS_NATIVE_MANIFEST}
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASMOS_NATIVE_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ${target_name})

  wasmos_add_ide_c_target(${target_name}_ide
    SOURCES ${WASMOS_NATIVE_SOURCES}
    INCLUDES ${WASMOS_NATIVE_INCLUDES}
  )
endfunction()

# The working .config lives in the BUILD directory, not the source tree.
#
# It used to live at ${CMAKE_SOURCE_DIR}/.config and was re-imported on every
# configure of every build dir, so one gitignored file in the repo root decided
# the runtime for all of them -- and beat -DWASMOS_WASM_RUNTIME_* on the command
# line, since kconfig import runs after and FORCEs the cache. Two build dirs
# configured with opposite flags could both come out WARP, and nothing said so.
#
# Now each build dir owns its config: it is seeded ONCE, then never re-read from
# the source tree, so trees cannot drift into each other. Kconfig writes
# .config.old next to it, which lands in the build dir too rather than
# littering the repo root.
#
# WASMOS_DOTCONFIG names the SEED, and may be a tracked defconfig
# (-DWASMOS_DOTCONFIG=configs/wasm3_smp_defconfig). It is never written to; the
# editing targets below always write the build-dir copy.
set(WASMOS_DOTCONFIG "" CACHE FILEPATH "Kconfig config to seed this build dir from (optional)")
set(WASMOS_BUILD_DOTCONFIG ${CMAKE_BINARY_DIR}/.config)
set(WASMOS_KCONFIG_IMPORT ${CMAKE_BINARY_DIR}/wasmos_kconfig.cmake)

if (NOT EXISTS ${WASMOS_BUILD_DOTCONFIG})
  # Seed order: an explicit seed, else the repo-root .config if the developer
  # still has one (so an existing default runtime is preserved rather than
  # silently flipped), else the tracked base defconfig.
  set(_wasmos_seed "")
  if (WASMOS_DOTCONFIG AND EXISTS ${WASMOS_DOTCONFIG})
    set(_wasmos_seed ${WASMOS_DOTCONFIG})
  elseif (EXISTS ${CMAKE_SOURCE_DIR}/.config)
    set(_wasmos_seed ${CMAKE_SOURCE_DIR}/.config)
  elseif (EXISTS ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig)
    set(_wasmos_seed ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig)
    # The base defconfig carries toolchains and IRQ mode but deliberately no
    # runtime or topology selection -- unlike configs/{wasm3,warp}_{single,smp}_
    # defconfig, which all set both. Seeding from it therefore leaves the
    # runtime on the code default rather than choosing one, so say so.
    message(STATUS "wasmos: configs/wasmos_defconfig selects no WASM runtime; "
                   "the code default applies. Pass "
                   "-DWASMOS_DOTCONFIG=configs/{wasm3,warp}_{single,smp}_defconfig to pin one.")
  endif ()
  if (_wasmos_seed)
    message(STATUS "wasmos: seeding ${WASMOS_BUILD_DOTCONFIG} from ${_wasmos_seed}")
    configure_file(${_wasmos_seed} ${WASMOS_BUILD_DOTCONFIG} COPYONLY)
  endif ()
elseif (WASMOS_DOTCONFIG AND EXISTS ${WASMOS_DOTCONFIG})
  # Re-seeding an already-configured dir would silently discard local kconfig
  # edits, so say what is being used instead of guessing.
  message(STATUS "wasmos: keeping existing ${WASMOS_BUILD_DOTCONFIG} "
                 "(delete it to re-seed from ${WASMOS_DOTCONFIG})")
endif ()

if (EXISTS ${WASMOS_BUILD_DOTCONFIG})
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_BUILD_DOTCONFIG}
            --output ${WASMOS_KCONFIG_IMPORT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE WASMOS_KCONFIG_IMPORT_RESULT
  )
  if (NOT WASMOS_KCONFIG_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to import ${WASMOS_BUILD_DOTCONFIG} into CMake cache.")
  endif ()
  include(${WASMOS_KCONFIG_IMPORT})
endif ()

# Ring3 shadow builds always use the wasm3 interpreter regardless of the
# kconfig runtime selection in .config (which selects WARP for the main build).
# Override here, after kconfig, so FORCE from kconfig is overridden in turn.
if (WASMOS_RING3_SHADOW)
  set(WASMOS_WASM_RUNTIME_WARP OFF CACHE BOOL "Use WARP JIT compiler" FORCE)
  set(WASMOS_WASM_RUNTIME_WASM3 ON  CACHE BOOL "Use wasm3 interpreter" FORCE)
endif ()

# WARP now always executes through the ring-3 path; remove the old user-facing
# runtime-mode toggle and gate the code directly on the WARP runtime choice.
unset(WASMOS_RING3 CACHE)

# WASM runtime selection must run AFTER the Kconfig import so that on a fresh
# cmake cache (e.g. the ring3 shadow build) the kconfig-selected runtime wins
# before CFLAGS_KERNEL is assembled.  On a warm cache the initialization block
# above has already run kconfig via the prior configure so the ordering only
# matters on the very first configure of a new build directory.
if (WASMOS_WASM_RUNTIME_WARP)
  set(WASM_RUNTIME_INCLUDE_DIR ${WARP_DIR})
  set(WASM_RUNTIME_DEFINE -DWASMOS_WASM_RUNTIME=1)
  set(BACKEND "x86_64" CACHE STRING "" FORCE)
  set(STATIC_LIB ON CACHE BOOL "" FORCE)
  add_subdirectory(${WARP_DIR} ${CMAKE_BINARY_DIR}/warp EXCLUDE_FROM_ALL)
else ()
  set(WASM_RUNTIME_INCLUDE_DIR ${WASM3_DIR})
  set(WASM_RUNTIME_DEFINE -DWASMOS_WASM_RUNTIME=0)
endif ()

# IRQ mode determination must run AFTER the Kconfig import so that
# WASMOS_IRQ_IOAPIC / WASMOS_IRQ_LAPIC reflect the .config values, not
# just the pre-import cmake defaults.
if (WASMOS_IRQ_IOAPIC)
  set(WASMOS_IRQ_MODE_VALUE 2)
elseif (WASMOS_IRQ_LAPIC)
  set(WASMOS_IRQ_MODE_VALUE 1)
else ()
  set(WASMOS_IRQ_MODE_VALUE 0)
endif ()
set(WASMOS_IRQ_MODE_DEFINE -DWASMOS_IRQ_MODE=${WASMOS_IRQ_MODE_VALUE})

# SMP requires I/O APIC — enforce the dependency even if .config is inconsistent.
if (WASMOS_SMP AND NOT WASMOS_IRQ_IOAPIC)
  message(WARNING "WASMOS_SMP requires WASMOS_IRQ_IOAPIC; disabling SMP")
  set(WASMOS_SMP OFF CACHE BOOL "Enable SMP" FORCE)
endif ()
if (WASMOS_SMP)
  set(WASMOS_SMP_VALUE 1)
  set(WASMOS_QEMU_SMP_COUNT 4)
  set(WASMOS_QEMU_SMP_ARGS "-smp" "4")
else ()
  set(WASMOS_SMP_VALUE 0)
  set(WASMOS_QEMU_SMP_COUNT 1)
  set(WASMOS_QEMU_SMP_ARGS "")
endif ()
set(WASMOS_SMP_DEFINE -DWASMOS_SMP=${WASMOS_SMP_VALUE})

# WARP's single-pass JIT emits modern x86-64 instructions (e.g. POPCNT/LZCNT/TZCNT
# for wasm i32.popcnt/clz/ctz) unconditionally, without a CPUID feature check. The
# QEMU default CPU model (qemu64) does not advertise those, so such instructions
# fault with #UD. Advertise a full modern CPU so the JIT-emitted instructions are
# valid (real x86-64 hardware has them). See docs/architecture/warp notes.
set(WASMOS_QEMU_CPU_ARGS "-cpu" "max")

# Wall-clock timeout (seconds) for the QEMU halt/boot smoke test. Defaults to
# 120 for fast local (KVM/HVF) runs; bump it for slow TCG environments such as
# CI runners without hardware acceleration: -DWASMOS_QEMU_TEST_TIMEOUT=600.
set(WASMOS_QEMU_TEST_TIMEOUT 120 CACHE STRING "QEMU halt-test timeout in seconds")

if (NOT DEFINED WASMOS_SCHED_CALLER_CPU_BIAS_INITIALIZED)
  set(WASMOS_SCHED_CALLER_CPU_BIAS ON CACHE BOOL "Enqueue woken threads on the waking CPU's ready queue" FORCE)
  set(WASMOS_SCHED_CALLER_CPU_BIAS_INITIALIZED ON CACHE BOOL "" FORCE)
endif ()
option(WASMOS_SCHED_CALLER_CPU_BIAS "Enqueue woken threads on the waking CPU's ready queue" ON)
if (WASMOS_SCHED_CALLER_CPU_BIAS)
  set(WASMOS_SCHED_CALLER_CPU_BIAS_VALUE 1)
else ()
  set(WASMOS_SCHED_CALLER_CPU_BIAS_VALUE 0)
endif ()
set(WASMOS_SCHED_CALLER_CPU_BIAS_DEFINE -DWASMOS_SCHED_CALLER_CPU_BIAS=${WASMOS_SCHED_CALLER_CPU_BIAS_VALUE})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WASMOS_WASM_CXX_FLAGS
  -fno-exceptions
  -fno-rtti
  -fno-threadsafe-statics
  -fno-use-cxa-atexit
)

set(INITFS_TOML ${CMAKE_SOURCE_DIR}/scripts/initfs.toml)
set(INITFS_IMG ${BUILD_DIR}/initfs.img)
set(INITFS_CONFIG_BIN ${BUILD_DIR}/initfs_config.bin)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # Avoid host -arch injection from environment/defaults.
  set(CMAKE_C_FLAGS "" CACHE STRING "Reset host C flags for freestanding build" FORCE)
  set(CMAKE_ASM_FLAGS "" CACHE STRING "Reset host ASM flags for freestanding build" FORCE)
  foreach(flag_var
          CMAKE_C_FLAGS CMAKE_ASM_FLAGS
          CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
          CMAKE_ASM_FLAGS_DEBUG CMAKE_ASM_FLAGS_RELEASE CMAKE_ASM_FLAGS_RELWITHDEBINFO CMAKE_ASM_FLAGS_MINSIZEREL)
    if (DEFINED ${flag_var})
      string(REPLACE "-arch arm64" "" ${flag_var} "${${flag_var}}")
      string(REPLACE "-arch;x86_64" "" ${flag_var} "${${flag_var}}")
      string(REPLACE "-arch x86_64" "" ${flag_var} "${${flag_var}}")
      set(${flag_var} "${${flag_var}}" CACHE STRING "Sanitized flags for freestanding build" FORCE)
    endif ()
  endforeach()
endif ()

execute_process(
  COMMAND ${CLANG} --version
  OUTPUT_VARIABLE CLANG_VERSION
  ERROR_VARIABLE CLANG_VERSION
)

if (CLANG_VERSION MATCHES "Apple clang")
  message(FATAL_ERROR "AppleClang cannot build UEFI targets. Install LLVM clang and set -DCLANG=/path/to/llvm/bin/clang.")
endif ()

if (NOT CLANG_VERSION MATCHES "clang")
  message(FATAL_ERROR "Unsupported compiler for UEFI build. Use LLVM clang.")
endif ()

get_filename_component(CLANG_BIN_DIR ${CLANG} DIRECTORY)
find_program(OBJCOPY_PATH ${OBJCOPY} HINTS ${CLANG_BIN_DIR} /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /opt/homebrew/bin /usr/local/bin)
if (NOT OBJCOPY_PATH)
  message(FATAL_ERROR "llvm-objcopy not found. Install LLVM and set -DOBJCOPY=/path/to/llvm-objcopy.")
endif ()
set(OBJCOPY ${OBJCOPY_PATH} CACHE STRING "Objcopy tool" FORCE)

set(LLD_HINTS ${CLANG_BIN_DIR} /opt/homebrew/opt/lld/bin /usr/local/opt/lld/bin /opt/homebrew/bin /usr/local/bin)
set(LLD_PATH "" CACHE FILEPATH "Resolved path to lld/ld.lld" FORCE)
set(LLD_CANDIDATES
  ${CLANG_BIN_DIR}/ld.lld
  /opt/homebrew/opt/lld/bin/ld.lld
  /usr/local/opt/lld/bin/ld.lld
  /opt/homebrew/bin/ld.lld
  /usr/local/bin/ld.lld
  ${CLANG_BIN_DIR}/lld
  /opt/homebrew/opt/lld/bin/lld
  /usr/local/opt/lld/bin/lld
  /opt/homebrew/bin/lld
  /usr/local/bin/lld
)
foreach(candidate IN LISTS LLD_CANDIDATES)
  if (EXISTS ${candidate})
    set(LLD_PATH ${candidate} CACHE FILEPATH "Resolved path to lld/ld.lld" FORCE)
    break()
  endif ()
endforeach()
if (NOT LLD_PATH)
  if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_program(LLD_PATH ld.lld HINTS ${LLD_HINTS})
    if (NOT LLD_PATH)
      find_program(LLD_PATH ${LLD} HINTS ${LLD_HINTS})
    endif ()
  else ()
    find_program(LLD_PATH ${LLD} HINTS ${LLD_HINTS})
    if (NOT LLD_PATH)
      find_program(LLD_PATH ld.lld HINTS ${LLD_HINTS})
    endif ()
  endif ()
endif ()
if (NOT LLD_PATH)
  message(FATAL_ERROR "lld/ld.lld not found. Install LLVM lld or set -DLLD=/path/to/lld.")
endif ()
get_filename_component(LLD_DIR ${LLD_PATH} DIRECTORY)

set(CFLAGS_COMMON -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -Wno-error=c23-extensions -g)
set(CFLAGS_EFI ${CFLAGS_COMMON} -target x86_64-unknown-uefi -fshort-wchar -mno-red-zone -B${CLANG_BIN_DIR} -include ${LIBC_DIR}/include/wasmos_cast.h)
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -target ${KERNEL_TARGET_TRIPLE} -mno-red-zone -mcmodel=kernel -m64 -fno-jump-tables -I${WASM_RUNTIME_INCLUDE_DIR} -I${KERNEL_DIR} -I${KERNEL_DIR}/include -I${DRIVER_WASM_DIR}/include -I${CMAKE_SOURCE_DIR}/abi/generated/c -B${LLD_DIR} -DWASMOS_ENABLE_PREEMPT_GUARD=1 -DWASMOS_ENABLE_SAFEPOINT=1 ${WASM_RUNTIME_DEFINE} -include ${LIBC_DIR}/include/wasmos_cast.h)
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  find_program(LLD_LINK_PATH ${LLD_LINK} HINTS ${LLD_HINTS})
  if (LLD_LINK_PATH)
    get_filename_component(LLD_LINK_DIR ${LLD_LINK_PATH} DIRECTORY)
    if (NOT LLD_LINK_DIR STREQUAL CLANG_BIN_DIR)
      set(CFLAGS_EFI ${CFLAGS_EFI} -B${LLD_LINK_DIR})
    endif ()
    set(LDFLAGS_EFI -fuse-ld=lld-link -Wl,/subsystem:efi_application -Wl,/entry:efi_main)
  else ()
    if (NOT LLD_DIR STREQUAL CLANG_BIN_DIR)
      set(CFLAGS_EFI ${CFLAGS_EFI} -B${LLD_DIR})
    endif ()
    set(LDFLAGS_EFI -fuse-ld=lld -Wl,-flavor,link -Wl,/subsystem:efi_application -Wl,/entry:efi_main)
  endif ()
else ()
  # The x86_64-unknown-uefi target links via lld-link (COFF), so it needs
  # MS-style options; GNU/ELF-style -Wl,--subsystem,10 -Wl,--entry,efi_main are
  # rejected by lld-link ("could not open '10'"). Match the macOS lld-link path.
  set(LDFLAGS_EFI -fuse-ld=lld-link -Wl,/subsystem:efi_application -Wl,/entry:efi_main)
endif ()

if (NOT OVMF_CODE)
  set(OVMF_CANDIDATES
    /usr/share/OVMF/OVMF_CODE.fd
    /opt/homebrew/share/OVMF/OVMF_CODE.fd
    /usr/local/share/OVMF/OVMF_CODE.fd
    /opt/homebrew/share/qemu/edk2-x86_64-code.fd
    /opt/homebrew/share/qemu/edk2-i386-code.fd
    /usr/local/share/qemu/edk2-x86_64-code.fd
    /usr/local/share/qemu/edk2-i386-code.fd
    /opt/homebrew/share/qemu/edk2-x86_64-secure-code.fd
    /usr/local/share/qemu/edk2-x86_64-secure-code.fd
    /opt/homebrew/share/edk2-ovmf/OVMF_CODE.fd
    /usr/local/share/edk2-ovmf/OVMF_CODE.fd
    /opt/homebrew/share/edk2/OVMF/OVMF_CODE.fd
    /usr/local/share/edk2/OVMF/OVMF_CODE.fd
  )
  foreach(candidate IN LISTS OVMF_CANDIDATES)
    if (EXISTS ${candidate})
      set(OVMF_CODE ${candidate} CACHE FILEPATH "Path to OVMF_CODE.fd" FORCE)
      break()
    endif ()
  endforeach()
endif ()

if (NOT OVMF_CODE)
  message(WARNING "OVMF_CODE.fd not found. Install OVMF/edk2 and set -DOVMF_CODE=/path/to/OVMF_CODE.fd to use run-qemu.")
endif ()

if (NOT OVMF_VARS AND OVMF_CODE)
  set(OVMF_VARS_CANDIDATES
    /usr/share/OVMF/OVMF_VARS.fd
    /opt/homebrew/share/OVMF/OVMF_VARS.fd
    /usr/local/share/OVMF/OVMF_VARS.fd
    /opt/homebrew/share/qemu/edk2-x86_64-vars.fd
    /opt/homebrew/share/qemu/edk2-i386-vars.fd
    /usr/local/share/qemu/edk2-x86_64-vars.fd
    /usr/local/share/qemu/edk2-i386-vars.fd
    /opt/homebrew/share/edk2-ovmf/OVMF_VARS.fd
    /usr/local/share/edk2-ovmf/OVMF_VARS.fd
    /opt/homebrew/share/edk2/OVMF/OVMF_VARS.fd
    /usr/local/share/edk2/OVMF/OVMF_VARS.fd
  )
  foreach(candidate IN LISTS OVMF_VARS_CANDIDATES)
    if (EXISTS ${candidate})
      set(OVMF_VARS ${candidate} CACHE FILEPATH "Path to OVMF_VARS.fd (optional)" FORCE)
      break()
    endif ()
  endforeach()
endif ()


add_executable(make_wasmos_app ${CMAKE_SOURCE_DIR}/scripts/make_wasmos_app.c)
target_compile_options(make_wasmos_app PRIVATE -O2)
set_target_properties(make_wasmos_app PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${BUILD_DIR}
)

# mkfs_wfs: the reference writer for the WFS format. Host-native, and built from
# the SAME sources the wasm driver reads through (wfs_format.h, wfs_crc32c.c,
# wfs_super.c) — a formatter with a private copy of the layout would agree with
# the driver only by review.
add_executable(mkfs_wfs
  ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/main.c
  ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
  ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c)
target_include_directories(mkfs_wfs PRIVATE
  ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
  ${DRIVER_WASM_DIR}/fs_wfs
  ${CMAKE_SOURCE_DIR}/abi/generated/c)
target_compile_options(mkfs_wfs PRIVATE -O2 -Wall -Wextra -Werror)
set_target_properties(mkfs_wfs PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${BUILD_DIR}
)

# ---------------------------------------------------------------------------
# Host-native WARP AOT pre-compiler tool (warp_aot)
# Built as a custom command because the main project uses a freestanding
# cross-compiler toolchain; warp_aot must use the host compiler with full
# libc and libstdc++ support.
#
# Common.cpp and Frontend.cpp unconditionally include all three backends
# (x86-64, aarch64, tricore) via template instantiations, so all three must
# be compiled and linked even though only x86-64 code is generated at runtime.
# The -DJIT_TARGET_X86_64 flag tells WARP's PlatformAdapter to use the x86-64
# backend as TBackend; the other backends are compiled but never selected.
# ---------------------------------------------------------------------------
if (WASMOS_WASM_RUNTIME_WARP)
  set(WARP_AOT_TOOL_DIR ${CMAKE_SOURCE_DIR}/src/tools/warp_aot)
  # Generated ABI tables that the WARP host tool compiles against. Listed so a
  # host-call signature change actually invalidates its objects; see the note on
  # the per-object rule below.
  set(WASMOS_ABI_GENERATED_HEADERS
    ${CMAKE_SOURCE_DIR}/abi/generated/c/wasmos_symbols_aot.inc
    ${CMAKE_SOURCE_DIR}/abi/generated/c/wasmos_symbols_warp.inc
    ${CMAKE_SOURCE_DIR}/abi/generated/c/wasmos_ring3_dispatch.inc
    ${CMAKE_SOURCE_DIR}/abi/generated/c/wasmos_imports.h
    ${CMAKE_SOURCE_DIR}/abi/generated/c/wasmos_status.h)

  set(WARP_AOT_SRCS
    ${WARP_DIR}/src/WasmModule/WasmModule.cpp
    ${WARP_DIR}/src/core/runtime/Runtime.cpp
    ${WARP_DIR}/src/core/runtime/ActiveMemoryManager.cpp
    ${WARP_DIR}/src/core/runtime/MemoryHelper.cpp
    ${WARP_DIR}/src/core/common/BinaryModule.cpp
    ${WARP_DIR}/src/core/common/VbExceptions.cpp
    ${WARP_DIR}/src/core/common/ExtendableMemory.cpp
    ${WARP_DIR}/src/utils/VirtualMemoryAllocator.cpp
    ${WARP_DIR}/src/utils/LinearMemoryAllocator.cpp
    ${WARP_DIR}/src/utils/OSAPIChecker.cpp
    ${WARP_DIR}/src/utils/SignalFunctionWrapper.cpp
    ${WARP_DIR}/src/utils/SignalFunctionWrapper_unix.cpp
    # RAIISignalHandler.cpp is excluded: it has a compile error when both
    # ACTIVE_STACK_OVERFLOW_CHECK=1 and LINEAR_MEMORY_BOUNDS_CHECKS=1 are set
    # (a struct sigaction 'sa' is declared inside a disabled #if block but then
    # referenced in !ACTIVE_DIV_CHECK, which is active). The kernel works around
    # this by providing os_api_checker_kernel.cpp; the host tool provides a stub
    # for RAIISignalHandler::restoreSignalHandler() in host_shim.cpp.
    ${WARP_DIR}/src/core/compiler/Compiler.cpp
    ${WARP_DIR}/src/core/compiler/backend/BackendBase.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_assembler.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_backend.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_call_dispatch.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_cc.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_instruction.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_relpatchobj.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_assembler.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_aux.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_backend.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_call_dispatch.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_cc.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_instruction.cpp
    ${WARP_DIR}/src/core/compiler/backend/aarch64/aarch64_relpatchobj.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_assembler.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_aux.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_backend.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_call_dispatch.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_cc.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_instruction.cpp
    ${WARP_DIR}/src/core/compiler/backend/tricore/tricore_relpatchobj.cpp
    ${WARP_DIR}/src/core/compiler/common/Common.cpp
    ${WARP_DIR}/src/core/compiler/common/CommonRegsPos.cpp
    ${WARP_DIR}/src/core/compiler/common/MemWriter.cpp
    ${WARP_DIR}/src/core/compiler/common/ModuleInfo.cpp
    ${WARP_DIR}/src/core/compiler/common/Stack.cpp
    ${WARP_DIR}/src/core/compiler/frontend/BytecodeReader.cpp
    ${WARP_DIR}/src/core/compiler/frontend/Frontend.cpp
    ${WARP_DIR}/src/core/compiler/frontend/ValidationStack.cpp
    ${WARP_AOT_TOOL_DIR}/host_mem_utils.cpp
    ${WARP_AOT_TOOL_DIR}/host_shim.cpp
    ${WARP_AOT_TOOL_DIR}/warp_aot.cpp
  )
  # -DJIT_TARGET_X86_64 selects the x86-64 backend as TBackend while still
  # compiling the aarch64/tricore backends needed by Common.cpp/Frontend.cpp.
  set(WARP_AOT_FLAGS
    -std=c++14 -O2
    -DJIT_TARGET_X86_64
    -DACTIVE_STACK_OVERFLOW_CHECK=1 -DLINEAR_MEMORY_BOUNDS_CHECKS=1
    -DVB_POSIX -DVB_WIN32_OR_POSIX -DNVALGRIND
    -I${WARP_DIR} -I${WARP_DIR}/src -I${CMAKE_SOURCE_DIR}/abi/generated/c
    -Wno-deprecated -Wno-unused-variable -Wno-macro-redefined
    -Wno-c++20-extensions -Wno-deprecated-copy -Wno-unused-parameter
    -Wno-unsequenced "-Wno-#pragma-messages"
  )
  # Compile each WARP source file to its own object file so the compiler cache
  # (ccache) can skip unchanged translation units on incremental builds.
  set(WARP_AOT_OBJ_DIR ${BUILD_DIR}/warp_aot_objs)
  set(WARP_AOT_OBJS "")
  foreach(_src ${WARP_AOT_SRCS})
    file(RELATIVE_PATH _rel ${CMAKE_SOURCE_DIR} ${_src})
    string(REPLACE "/" "_" _obj_name "${_rel}")
    string(REPLACE ".cpp" ".o" _obj_name "${_obj_name}")
    set(_obj "${WARP_AOT_OBJ_DIR}/${_obj_name}")
    # The generated ABI tables are a DEPENDENCY, not just an include path.
    # warp_aot.cpp #includes wasmos_symbols_aot.inc, which names a stub per
    # host call BY ARITY (stub_i1, stub_i2, ...). Change a host call's
    # signature and that file changes -- but with only ${_src} in DEPENDS the
    # object is not rebuilt, so the tool keeps advertising the OLD arity while
    # guest modules import the new one, and AOT compilation fails with
    # "Imported symbol could not be found" for whichever module uses that call.
    # That cost one abandoned io_in32 conversion before the cause was found.
    #
    # FIXME: DEPENDS lists no libs/warp headers, and hand-rolled custom commands
    # get no implicit header scanning. A subtree pull that touches only WARP
    # headers (Compiler.hpp, BumpAllocator.hpp, *_relpatchobj.hpp -- all of which
    # the 4.1.1 -> 4.2.0 bump changed) rebuilds only the TUs whose .cpp changed,
    # so warp_aot links objects compiled against two different header versions.
    # Until this tracks the WARP headers, delete <build>/warp_aot_objs by hand
    # after every subtree pull.
    add_custom_command(
      OUTPUT "${_obj}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${WARP_AOT_OBJ_DIR}"
      COMMAND ${CLANGXX_PATH} ${WARP_AOT_FLAGS} -c "${_src}" -o "${_obj}"
      DEPENDS "${_src}" ${WASMOS_ABI_GENERATED_HEADERS}
      COMMENT "Compiling warp_aot: ${_rel}"
      VERBATIM
    )
    list(APPEND WARP_AOT_OBJS "${_obj}")
  endforeach()
  set(WARP_AOT_BIN ${BUILD_DIR}/warp_aot)
  add_custom_command(
    OUTPUT ${WARP_AOT_BIN}
    COMMAND ${CLANGXX_PATH} ${WARP_AOT_OBJS} -o ${WARP_AOT_BIN}
    DEPENDS ${WARP_AOT_OBJS}
    COMMENT "Linking warp_aot host tool"
    VERBATIM
  )
  # NOTE: the target name must differ from the produced file's build-relative
  # path (${BUILD_DIR}/warp_aot, i.e. "warp_aot" under CMAKE_BINARY_DIR).  If
  # they match, the Ninja generator emits both a phony rule and a build rule
  # named "warp_aot" -> "multiple rules generate warp_aot" + a phony self-cycle
  # (the Make generator tolerates this; Ninja/CLion does not).  Hence the
  # "_tool" suffix on the target while the binary keeps its name.
  add_custom_target(warp_aot_tool DEPENDS ${WARP_AOT_BIN})

  # Execute a small wasm32 coroutine fixture through WARP on the build host.
  # This deliberately uses the host ISA, unlike warp_aot which always emits
  # x86-64 code for the kernel target.
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(WARP_HOST_TEST_TARGET_FLAG -DJIT_TARGET_X86_64)
  endif()
  if(DEFINED WARP_HOST_TEST_TARGET_FLAG)
    set(WARP_HOST_TEST_FIXTURE ${BUILD_DIR}/test_wasm_coroutine_fixture.wasm)
    add_custom_command(
      OUTPUT ${WARP_HOST_TEST_FIXTURE}
      # -nostdlib is not a promise that the compiler emits no libc calls: it may
      # still lower an aggregate initialiser to memset (clang 18 does for this
      # fixture's `= {0}` structs, clang 20+ inlines it). Every other wasm target
      # here links the sysroot's libc.a and so already satisfies that; this one
      # linked nothing, and only ever built on an x86_64 host with WARP enabled --
      # a combination that did not occur until WARP became the default runtime.
      # Link the real libc string/ctype implementations rather than
      COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding
              -Wl,--no-entry -Wl,--strip-all -Wl,--export=wasmos_coroutine_wasm_test
              -I${LIBSYS_WASM_DIR}/include
              -I${LIBC_DIR}/include
              ${CMAKE_SOURCE_DIR}/tests/unit/wasm_coroutine_warp_fixture.c
              ${LIBSYS_WASM_DIR}/coroutine_wasm.c
              ${LIBC_DIR}/src/string.c
              ${LIBC_DIR}/src/ctype.c
              -o ${WARP_HOST_TEST_FIXTURE}
      DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/wasm_coroutine_warp_fixture.c
              ${LIBSYS_WASM_DIR}/coroutine_wasm.c
              ${LIBC_DIR}/src/string.c
              ${LIBC_DIR}/src/ctype.c
              ${LIBSYS_WASM_DIR}/include/wasmos/coroutine_wasm.h
      COMMENT "Building wasm32 coroutine fixture for host WARP"
      VERBATIM)
    set(WARP_HOST_TEST_SRCS ${WARP_AOT_SRCS})
    list(REMOVE_ITEM WARP_HOST_TEST_SRCS ${WARP_AOT_TOOL_DIR}/warp_aot.cpp)
    list(APPEND WARP_HOST_TEST_SRCS ${CMAKE_SOURCE_DIR}/tests/unit/test_warp_wasm_coroutine.cpp)
    set(WARP_HOST_TEST_FLAGS ${WARP_AOT_FLAGS})
    list(REMOVE_ITEM WARP_HOST_TEST_FLAGS -DJIT_TARGET_X86_64)
    list(APPEND WARP_HOST_TEST_FLAGS ${WARP_HOST_TEST_TARGET_FLAG})
    set(WARP_HOST_TEST_OBJ_DIR ${BUILD_DIR}/warp_host_test_objs)
    set(WARP_HOST_TEST_OBJS "")
    foreach(_src ${WARP_HOST_TEST_SRCS})
      file(RELATIVE_PATH _rel ${CMAKE_SOURCE_DIR} ${_src})
      string(REPLACE "/" "_" _obj_name "${_rel}")
      string(REPLACE ".cpp" ".o" _obj_name "${_obj_name}")
      set(_obj "${WARP_HOST_TEST_OBJ_DIR}/${_obj_name}")
      add_custom_command(
        OUTPUT "${_obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WARP_HOST_TEST_OBJ_DIR}"
        COMMAND ${CLANGXX_PATH} ${WARP_HOST_TEST_FLAGS} -c "${_src}" -o "${_obj}"
        DEPENDS "${_src}"
        COMMENT "Compiling WARP host test: ${_rel}"
        VERBATIM)
      list(APPEND WARP_HOST_TEST_OBJS "${_obj}")
    endforeach()
    set(WARP_HOST_TEST_BIN ${BUILD_DIR}/test_warp_wasm_coroutine)
    add_custom_command(
      OUTPUT ${WARP_HOST_TEST_BIN}
      COMMAND ${CLANGXX_PATH} ${WARP_HOST_TEST_OBJS} -o ${WARP_HOST_TEST_BIN}
      DEPENDS ${WARP_HOST_TEST_OBJS}
      COMMENT "Linking WARP host coroutine test"
      VERBATIM)
    add_custom_target(warp_wasm_coroutine_test
      COMMAND ${WARP_HOST_TEST_BIN} ${WARP_HOST_TEST_FIXTURE}
      DEPENDS ${WARP_HOST_TEST_BIN} ${WARP_HOST_TEST_FIXTURE}
      COMMENT "Running wasm coroutine fixture through host WARP"
      VERBATIM)
  endif()
endif ()

# wasmos_maybe_aot_pack(MANIFEST WASM APP)
#
# If WARP is enabled and MANIFEST contains "aot = true", compile WASM to a
# WARP native binary with the warp_aot host tool and embed it in APP using
# make_wasmos_app --compiled.  Otherwise pack APP from WASM without AOT.
#
# Only call this after set(WASMOS_APP_PACKER ...) — it references that var.
# Extra make-time dependencies (e.g. a .wam target) may be appended after APP.
function(wasmos_maybe_aot_pack MANIFEST WASM APP)
  set(_extra_deps ${ARGN})
  set(_do_aot OFF)
  if (WASMOS_WASM_RUNTIME_WARP AND NOT WASMOS_WARP_AOT_DISABLED
      AND DEFINED WARP_AOT_BIN AND EXISTS "${MANIFEST}")
    file(READ "${MANIFEST}" _meta)
    string(FIND "${_meta}" "aot = true" _pos)
    if (NOT _pos EQUAL -1)
      set(_do_aot ON)
    endif()
  endif()

  get_filename_component(_name "${APP}" NAME_WE)

  if (_do_aot)
    set(_warpbin "${BUILD_DIR}/${_name}.warpbin")
    set(_aot_cache_dir "${CMAKE_SOURCE_DIR}/.cache/warp_aot")
    add_custom_command(
      OUTPUT "${_warpbin}"
      # Use the caching cmake script so unchanged WASM files are not recompiled
      # across clean builds.  Depend on the warp_aot BINARY file (for cache
      # invalidation when the tool changes) AND on the warp_aot_tool TARGET.  The
      # target dependency is what makes a fresh build order the host tool ahead
      # of the apps that consume it: this custom command is created in an app
      # subdirectory, and a cross-directory dependency on a custom-command
      # OUTPUT file alone gives the Make generator no rule to build the tool
      # ("No rule to make target 'warp_aot'").  Depending on the custom_target
      # creates a target-level ordering dependency (not a file-level re-run
      # trigger), so it does not mark this output always-stale.
      COMMAND ${CMAKE_COMMAND}
              "-DWARP_AOT=${WARP_AOT_BIN}"
              "-DWASM=${WASM}"
              "-DWARPBIN=${_warpbin}"
              "-DCACHE_DIR=${_aot_cache_dir}"
              -P "${CMAKE_SOURCE_DIR}/cmake/warp_aot_cached.cmake"
      DEPENDS "${WASM}" "${WARP_AOT_BIN}" warp_aot_tool ${_extra_deps}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "AOT compiling ${_name}"
      VERBATIM
    )
    add_custom_command(
      OUTPUT "${APP}"
      COMMAND "${WASMOS_APP_PACKER}"
              --manifest "${MANIFEST}"
              --in "${WASM}"
              --compiled "${_warpbin}"
              --out "${APP}"
      DEPENDS "${WASM}" "${_warpbin}" make_wasmos_app "${MANIFEST}" ${_extra_deps}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Packing ${_name} (with AOT binary)"
      VERBATIM
    )
  else()
    add_custom_command(
      OUTPUT "${APP}"
      COMMAND "${WASMOS_APP_PACKER}"
              --manifest "${MANIFEST}"
              --in "${WASM}"
              --out "${APP}"
      DEPENDS "${WASM}" make_wasmos_app "${MANIFEST}" ${_extra_deps}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Packing ${_name}"
      VERBATIM
    )
  endif()
endfunction()

set(WASMOS_APP_PACKER $<TARGET_FILE:make_wasmos_app>)

# The staged SDK (target `wasmos-sdk`) repackages this toolchain for use outside
# the repository.  Included here because it needs WASMOS_APP_PACKER.
include(${CMAKE_SOURCE_DIR}/cmake/WasmosSdk.cmake)

# The initfs payloads are whatever `scripts/initfs.toml` lists, so derive the
# rule's dependencies from that manifest instead of restating them here.  A
# hand-written list silently goes stale: framebuffer_pci.wap was embedded in
# initfs but missing from the list, so editing the framebuffer driver rebuilt the
# .wap and left the booted image untouched -- a change that appears to do nothing
# and a test that measures the previous build.  make_initfs.py resolves every
# `source` against the build dir; the devmgr rules file is produced by this rule's
# own copy step, so it is excluded here and tracked through its source-tree path.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${INITFS_TOML})
file(STRINGS ${INITFS_TOML} _initfs_source_lines REGEX "^[ \t]*source[ \t]*=")
set(WASMOS_INITFS_PAYLOADS "")
foreach(_line IN LISTS _initfs_source_lines)
  string(REGEX REPLACE "^[ \t]*source[ \t]*=[ \t]*\"([^\"]+)\".*$" "\\1" _payload "${_line}")
  if (NOT _payload STREQUAL "devmgr_rules_init_default.rules")
    list(APPEND WASMOS_INITFS_PAYLOADS ${BUILD_DIR}/${_payload})
  endif()
endforeach()
list(REMOVE_DUPLICATES WASMOS_INITFS_PAYLOADS)

add_custom_command(
  OUTPUT ${INITFS_IMG} ${INITFS_CONFIG_BIN}
  COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_SOURCE_DIR}/scripts/initfs/devmgr/rules/default.rules
          ${BUILD_DIR}/devmgr_rules_init_default.rules
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/make_initfs.py
          --manifest ${INITFS_TOML}
          --build-dir ${BUILD_DIR}
          --output ${INITFS_IMG}
          --config-output ${INITFS_CONFIG_BIN}
  DEPENDS ${CMAKE_SOURCE_DIR}/scripts/make_initfs.py
          ${INITFS_TOML}
          ${CMAKE_SOURCE_DIR}/scripts/initfs/devmgr/rules/default.rules
          ${WASMOS_INITFS_PAYLOADS}
          make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building initfs image"
  VERBATIM
)
add_custom_target(make_initfs DEPENDS ${INITFS_IMG} ${INITFS_CONFIG_BIN})

if (OVMF_VARS AND NOT EXISTS ${OVMF_VARS})
  message(WARNING "OVMF_VARS '${OVMF_VARS}' not found; continuing without vars.")
  set(OVMF_VARS "" CACHE FILEPATH "Path to OVMF_VARS.fd (optional)" FORCE)
endif ()

add_subdirectory(src/boot)
add_subdirectory(src/drivers)
add_subdirectory(examples/c)
add_subdirectory(src/services)
add_subdirectory(src/utils)
add_subdirectory(examples/assemblyscript)
add_subdirectory(examples/rust)
add_subdirectory(examples/go)
add_subdirectory(examples/zig)
add_subdirectory(src/kernel)

get_property(WASMOS_WASM_APP_TARGETS GLOBAL PROPERTY WASMOS_WASM_APP_TARGETS)
get_property(WASMOS_KERNEL_BLOB_TARGETS GLOBAL PROPERTY WASMOS_KERNEL_BLOB_TARGETS)
# Prerequisites shared by every target that can build a wasm module.
#
# `make_wasmos_app` packs the .wap. `wasmos-sdk` stages the sysroot: each language's
# driver resolves headers, archives and crt1 out of it, so it has to exist before any
# app compiles. That one is a TARGET-level dependency by necessity -- the stage's
# outputs are custom commands in THIS directory, and a subdirectory cannot name
# another directory's custom-command output as a file dependency (a clean tree
# reports "No rule to make target 'sdk-obj/sysroot.stamp'"). Per-app freshness comes
# from WASMOS_SDK_ARCHIVE_SOURCES, which are ordinary source files.
#
# The loop is not cosmetic: `add_dependencies(<list> dep)` reads the FIRST element as
# the target and every remaining one as a dependency, so passing a list gives the
# dependency to one target and silently skips the rest.
#
# Blob targets need it too. CMake copies a custom command into each target that
# consumes its output, so a blob target depending on a module file carries its own
# copy of the rule and would otherwise run a driver before the sysroot exists.
foreach (_wasm_target IN LISTS WASMOS_WASM_APP_TARGETS WASMOS_KERNEL_BLOB_TARGETS)
  add_dependencies(${_wasm_target} make_wasmos_app wasmos-sdk)
endforeach ()
set(WASM_DRIVER_BLOB_DEPS "")
list(APPEND WASM_DRIVER_BLOB_DEPS make_wasmos_app)
if (WASMOS_WASM_APP_TARGETS)
  list(APPEND WASM_DRIVER_BLOB_DEPS ${WASMOS_WASM_APP_TARGETS})
endif ()
if (WASMOS_KERNEL_BLOB_TARGETS)
  list(APPEND WASM_DRIVER_BLOB_DEPS ${WASMOS_KERNEL_BLOB_TARGETS})
endif ()
if (WASM_DRIVER_BLOB_DEPS)
  add_custom_target(wasm_driver_blobs DEPENDS ${WASM_DRIVER_BLOB_DEPS})
else ()
  add_custom_target(wasm_driver_blobs)
endif ()
add_dependencies(kernel wasm_driver_blobs)

# QEMU helper
set(QEMU_GDB_PORT 1234 CACHE STRING "QEMU gdb port for run-qemu-debug")
set(WASMOS_QEMU_NIC_MODEL "virtio-net-pci" CACHE STRING
    "QEMU NIC model for developer/test runs (for example virtio-net-pci or e1000)")
# QEMU netdev backend spec (everything after `-netdev`). Defaults to user-mode
# (SLIRP) NAT with QEMU's built-in DHCP server. Override to bridge onto a real
# network via the WASMOS_QEMU_NETDEV env var, then re-run `cmake -S . -B build`
# so these run-qemu* targets pick it up. The spec MUST keep `id=net0` so the
# `-device ...,netdev=net0` pairing below still matches. Examples (macOS vmnet
# needs sudo; Linux tap needs a preconfigured tapN):
#   macOS bridge to LAN:  vmnet-bridged,id=net0,ifname=en0
#   macOS Apple NAT+DHCP:  vmnet-shared,id=net0
#   Linux tap:             tap,id=net0,ifname=tap0,script=no,downscript=no
set(WASMOS_QEMU_NETDEV "$ENV{WASMOS_QEMU_NETDEV}")
if(WASMOS_QEMU_NETDEV STREQUAL "")
  set(WASMOS_QEMU_NETDEV "user,id=net0")
endif()
separate_arguments(WASMOS_QEMU_NETDEV_SPEC UNIX_COMMAND "${WASMOS_QEMU_NETDEV}")
set(WASMOS_QEMU_NETDEV_ARGS
  -netdev ${WASMOS_QEMU_NETDEV_SPEC}
  -device ${WASMOS_QEMU_NIC_MODEL},netdev=net0
  # Entropy source for the virtio-rng driver (transitional 1AF4:1005).
  -device virtio-rng-pci
)
# Restores this build directory's config from the defconfig it was created with,
# falling back to the shared base. It used to hardcode configs/wasmos_defconfig,
# which meant running it in a tree seeded from, say, configs/warp_smp_defconfig
# silently replaced that tree's config with one selecting NO runtime -- a
# "reset to defaults" that quietly changed which runtime the tree built.
if (WASMOS_DOTCONFIG AND EXISTS ${WASMOS_DOTCONFIG})
  set(_wasmos_reseed ${WASMOS_DOTCONFIG})
else ()
  set(_wasmos_reseed ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig)
endif ()
add_custom_target(kconfig-defconfig
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${_wasmos_reseed}
          ${WASMOS_BUILD_DOTCONFIG}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
          --input ${WASMOS_BUILD_DOTCONFIG}
          --output ${WASMOS_KCONFIG_IMPORT}
  COMMENT "Seeding ${WASMOS_BUILD_DOTCONFIG} from ${_wasmos_reseed}"
  VERBATIM
)
# Prefer kconfiglib's "menuconfig" (installed via pip install kconfiglib) for the
# full curses TUI. USES_TERMINAL gives it a proper TTY so it works from cmake.
# Derive the search hint from Python3_EXECUTABLE so a venv-installed menuconfig
# is found even when the venv is not on the cmake-time PATH.
get_filename_component(_PYTHON_BIN_DIR ${Python3_EXECUTABLE} DIRECTORY)
find_program(WASMOS_MENUCONFIG_BIN
  NAMES menuconfig nconfig kconfig-mconf mconf
  HINTS ${_PYTHON_BIN_DIR}
)
if (WASMOS_MENUCONFIG_BIN)
  add_custom_target(menuconfig
    COMMAND ${CMAKE_COMMAND} -E env KCONFIG_CONFIG=${WASMOS_BUILD_DOTCONFIG}
            ${WASMOS_MENUCONFIG_BIN} ${CMAKE_SOURCE_DIR}/Kconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_BUILD_DOTCONFIG}
            --output ${WASMOS_KCONFIG_IMPORT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run menuconfig and import .config into CMake cache"
    USES_TERMINAL
    VERBATIM
  )
else ()
  add_custom_target(menuconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfiglib_menuconfig.py
            --kconfig ${CMAKE_SOURCE_DIR}/Kconfig
            --config ${WASMOS_BUILD_DOTCONFIG}
            --defconfig ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_BUILD_DOTCONFIG}
            --output ${WASMOS_KCONFIG_IMPORT}
    COMMENT "Run menuconfig and import .config into CMake cache"
    USES_TERMINAL
    VERBATIM
  )
endif ()
add_custom_target(kconfiglib-menuconfig
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfiglib_menuconfig.py
          --kconfig ${CMAKE_SOURCE_DIR}/Kconfig
          --config ${WASMOS_BUILD_DOTCONFIG}
          --defconfig ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
          --input ${WASMOS_BUILD_DOTCONFIG}
          --output ${WASMOS_KCONFIG_IMPORT}
  COMMENT "Run kconfiglib interactive config editor"
  USES_TERMINAL
  VERBATIM
)
if (AS_ENABLE)
  set(AS_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${AS_HELLO_APP} ${BUILD_DIR}/esp/apps/hello_as.wap
  )
  set(AS_MINESWEEPER_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${AS_MINESWEEPER_APP} ${BUILD_DIR}/esp/apps/minesweeper.wap
  )
endif ()
set(DATE_TOOL_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DATE_TOOL_APP} ${BUILD_DIR}/esp/system/utils/date.wap
)
set(SCHED_INFO_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SCHED_INFO_APP} ${BUILD_DIR}/esp/system/utils/sched_info.wap
)
if (TARGET ps_util)
  set(PS_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PS_APP} ${BUILD_DIR}/esp/system/utils/ps.wap
  )
endif ()
if (TARGET cat_util)
  set(CAT_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CAT_APP} ${BUILD_DIR}/esp/system/utils/cat.wap
  )
endif ()
if (TARGET sysinfo_util)
  set(SYSINFO_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SYSINFO_APP} ${BUILD_DIR}/esp/system/utils/sysinfo.wap
  )
endif ()
if (TARGET explorer_util)
  set(EXPLORER_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_EXPLORER_APP} ${BUILD_DIR}/esp/system/utils/explorer.wap
  )
endif ()
if (TARGET ip_util)
  set(IP_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_IP_APP} ${BUILD_DIR}/esp/system/utils/ip.wap
  )
endif ()
if (TARGET host_util)
  set(HOST_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HOST_APP} ${BUILD_DIR}/esp/system/utils/host.wap
  )
endif ()
if (TARGET curl_util)
  set(CURL_UTIL_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CURL_APP} ${BUILD_DIR}/esp/system/utils/curl.wap
  )
endif ()
set(HELLO_C_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_C_APP} ${BUILD_DIR}/esp/apps/hello_c.wap
)
set(CHARDEV_PREEMPT_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_PREEMPT_APP} ${BUILD_DIR}/esp/apps/chardev_preempt.wap
)
set(INIT_SMOKE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_SMOKE_APP} ${BUILD_DIR}/esp/apps/init_smoke.wap
)
set(FS_OPEN_SMOKE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FS_OPEN_SMOKE_APP} ${BUILD_DIR}/esp/apps/fs_open_smoke.wap
)
set(FS_WRITE_SMOKE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FS_WRITE_SMOKE_APP} ${BUILD_DIR}/esp/apps/fs_write_smoke.wap
)
set(NATIVE_CALL_MIN_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_NATIVE_CALL_MIN_APP} ${BUILD_DIR}/esp/apps/native_call_min.wap
)
set(NATIVE_CALL_SMOKE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_NATIVE_CALL_SMOKE_APP} ${BUILD_DIR}/esp/apps/native_call_smoke.wap
)
add_custom_command(
  OUTPUT ${WAMOS_SCRIPT_DATA_BIN}
  COMMAND ${CMAKE_COMMAND} -E copy ${WAMOS_SCRIPT_DATA_SRC} ${WAMOS_SCRIPT_DATA_BIN}
  DEPENDS ${WAMOS_SCRIPT_DATA_SRC}
  COMMENT "Copying wamos-script sample initfs data"
  VERBATIM
)
add_custom_target(wamos_script_initfs_data DEPENDS ${WAMOS_SCRIPT_DATA_BIN})
set(GFX_SMOKE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_GFX_SMOKE_APP} ${BUILD_DIR}/esp/apps/gfx_smoke.wap
)
set(IRQ_ROUTE_DENY_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_IRQ_ROUTE_DENY_APP} ${BUILD_DIR}/esp/apps/irq_route_deny.wap
)
set(IRQ_ROUTE_ALLOW_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_IRQ_ROUTE_ALLOW_APP} ${BUILD_DIR}/esp/apps/irq_route_allow.wap
)
set(SHMEM_OWNER_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E rm -f ${BUILD_DIR}/esp/apps/shmem_owner.wap ${BUILD_DIR}/esp/apps/shmem_target.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SHMEM_OWNER_APP} ${BUILD_DIR}/esp/apps/shmownr.wap
)
set(SHMEM_TARGET_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SHMEM_TARGET_APP} ${BUILD_DIR}/esp/apps/shmtgt.wap
)
set(MENU_BAR_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_MENU_BAR_APP} ${BUILD_DIR}/esp/apps/menu_bar.wap
)
set(NET_SMOKE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_NET_SMOKE_APP} ${BUILD_DIR}/esp/apps/net_smoke.wap
)
set(NET_UDP_ECHO_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_NET_UDP_ECHO_APP} ${BUILD_DIR}/esp/apps/net_udp_echo.wap
)
set(NET_TCP_ECHO_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_NET_TCP_ECHO_APP} ${BUILD_DIR}/esp/apps/net_tcp_echo.wap
)
set(NET_TCP_SERVER_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_NET_TCP_SERVER_APP} ${BUILD_DIR}/esp/apps/net_tcp_server.wap
)
set(LARGE_READ_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/large_read.txt ${BUILD_DIR}/esp/large_read.txt
)
set(WRITE_SMOKE_FILE_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/write_smoke.txt ${BUILD_DIR}/esp/write_smoke.txt
)
# Keep hardware drivers on the ESP instead of embedding them in initfs;
# device-manager loads them by name after fs-fat is available.
set(INITFS_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${INITFS_IMG} ${BUILD_DIR}/esp/initfs.img
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/devmgr
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${BUILD_DIR}/esp/system/devmgr/rules
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/scripts/system/devmgr/rules
          ${BUILD_DIR}/esp/system/devmgr/rules
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${BUILD_DIR}/esp/system/keymaps
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/scripts/system/keymaps
          ${BUILD_DIR}/esp/system/keymaps
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/fonts
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/utils
  COMMAND ${CMAKE_COMMAND} -E rm -f ${BUILD_DIR}/esp/system/fonts/noto_serif.ttf
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/system/fonts/Roboto-Regular.ttf ${BUILD_DIR}/esp/system/fonts/roboto.ttf
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/system/fonts/RobotoMono-Regular.ttf ${BUILD_DIR}/esp/system/fonts/roboto_mono.ttf
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/system/fonts/RobotoSerif-Regular.ttf ${BUILD_DIR}/esp/system/fonts/roboto_serif.ttf
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SERIAL_APP} ${BUILD_DIR}/esp/system/drivers/serial.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_KEYBOARD_APP} ${BUILD_DIR}/esp/system/drivers/keyboard.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_MOUSE_APP} ${BUILD_DIR}/esp/system/drivers/mouse.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_RTC_APP} ${BUILD_DIR}/esp/system/drivers/rtc.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FRAMEBUFFER_APP} ${BUILD_DIR}/esp/system/drivers/framebuf.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FRAMEBUFFER_PCI_APP} ${BUILD_DIR}/esp/system/drivers/framebuf_pci.wap
  ${DATE_TOOL_COPY_CMD}
  ${SCHED_INFO_COPY_CMD}
  ${PS_UTIL_COPY_CMD}
  ${CAT_UTIL_COPY_CMD}
  ${SYSINFO_UTIL_COPY_CMD}
  ${EXPLORER_UTIL_COPY_CMD}
  ${IP_UTIL_COPY_CMD}
  ${HOST_UTIL_COPY_CMD}
  ${CURL_UTIL_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/scripts/system/sysinit.rc
      ${BUILD_DIR}/esp/system/sysinit.rc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/net
  COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/scripts/system/net/interfaces
      ${BUILD_DIR}/esp/system/net/interfaces
  # TLS CA trust store (milestone C). scripts/fetch-ca-certs.sh downloads and
  # pins the real Mozilla/curl.se bundle to
  # scripts/system/net/certificates/ca-certs.pem; copy it into the ESP when
  # present so it lands at guest /system/net/certificates/ca-certs.pem. Fall back
  # to an empty placeholder when it has not been fetched so the build never
  # depends on the network (net-stack then logs "no CA trust store" and TLS opens
  # fail; the hermetic verify test overwrites this file with its own test CA).
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/net/certificates
  COMMAND sh ${CMAKE_SOURCE_DIR}/scripts/copy-ca-into-esp.sh
      ${CMAKE_SOURCE_DIR}/scripts/system/net/certificates/ca-certs.pem
      ${BUILD_DIR}/esp/system/net/certificates/ca-certs.pem
  COMMAND ${CMAKE_COMMAND} -E make_directory ${USERFS_DIR}
)
if (RUST_ENABLE)
  set(RUST_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_RUST_APP} ${BUILD_DIR}/esp/apps/hello_rust.wap
  )
  set(TETRIS_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_TETRIS_RUST_APP} ${BUILD_DIR}/esp/apps/tetris.wap
  )
endif ()
if (GO_AVAILABLE)
  set(GO_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_GO_APP} ${BUILD_DIR}/esp/apps/hello_go.wap
  )
endif ()
if (ZIG_AVAILABLE)
  set(ZIG_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_ZIG_APP} ${BUILD_DIR}/esp/apps/hello_zig.wap
  )
  set(ZIG_CALCULATOR_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CALCULATOR_APP} ${BUILD_DIR}/esp/apps/calculator.wap
  )
endif ()

# Rebuilt whenever the content or the formatter changes: the guest mounts this
# image, so a stale one is a test passing against last build's filesystem.
file(GLOB_RECURSE WFS_CONTENT_FILES CONFIGURE_DEPENDS "${WFS_CONTENT_DIR}/*")
add_custom_command(
  OUTPUT ${WFS_IMAGE}
  COMMAND ${BUILD_DIR}/mkfs_wfs --quiet
          --uuid 5746532d74654573742d766f6c756d65
          --populate ${WFS_CONTENT_DIR}
          ${WFS_IMAGE} 16M
  DEPENDS mkfs_wfs ${WFS_CONTENT_FILES}
  COMMENT "Building the WFS test volume with mkfs_wfs"
  VERBATIM)
add_custom_target(wfs_image DEPENDS ${WFS_IMAGE})

set(WASMOS_ESP_POPULATE_CMDS
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/devices
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
  ${INITFS_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_SERVER_APP} ${BUILD_DIR}/esp/system/drivers/chardev_server.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_WFS_APP} ${BUILD_DIR}/esp/system/drivers/fs_wfs.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_GFX_COMPOSITOR_APP} ${BUILD_DIR}/esp/system/services/gfxcomp.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FONT_SERVICE_APP} ${BUILD_DIR}/esp/system/services/fontsvc.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ACPI_BUS_APP} ${BUILD_DIR}/esp/system/services/acpi_bus.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardevc.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VIRTIO_NET_APP} ${BUILD_DIR}/esp/system/drivers/virtio_net.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VIRTIO_RNG_APP} ${BUILD_DIR}/esp/system/drivers/virtio_rng.wap
  ${AS_HELLO_COPY_CMD}
  ${AS_MINESWEEPER_COPY_CMD}
  ${HELLO_C_COPY_CMD}
  ${SDK_HELLO_COPY_CMD}
  ${SDK_ZIG_HELLO_COPY_CMD}
  ${SDK_AS_HELLO_COPY_CMD}
  ${SDK_RUST_HELLO_COPY_CMD}
  ${SDK_GO_HELLO_COPY_CMD}
  ${CHARDEV_PREEMPT_COPY_CMD}
  ${INIT_SMOKE_COPY_CMD}
  ${FS_OPEN_SMOKE_COPY_CMD}
  ${FS_WRITE_SMOKE_COPY_CMD}
  ${NATIVE_CALL_MIN_COPY_CMD}
  ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${GFX_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
  ${MENU_BAR_COPY_CMD}
  ${NET_SMOKE_COPY_CMD}
  ${NET_UDP_ECHO_COPY_CMD}
  ${NET_TCP_ECHO_COPY_CMD}
  ${NET_TCP_SERVER_COPY_CMD}
  ${IRQ_ROUTE_DENY_COPY_CMD}
  ${IRQ_ROUTE_ALLOW_COPY_CMD}
  ${RUST_HELLO_COPY_CMD}
  ${TETRIS_COPY_CMD}
  ${GO_HELLO_COPY_CMD}
  ${ZIG_HELLO_COPY_CMD}
  ${ZIG_CALCULATOR_COPY_CMD}
  ${LARGE_READ_COPY_CMD}
  ${WRITE_SMOKE_FILE_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
)

if (OVMF_VARS AND OVMF_CODE)
  if (OVMF_VARS MATCHES "i386" AND OVMF_CODE MATCHES "x86_64")
    message(WARNING "OVMF_VARS appears to be i386 while OVMF_CODE is x86_64; ignoring OVMF_VARS to avoid boot hangs.")
    set(OVMF_VARS "" CACHE FILEPATH "Path to OVMF_VARS.fd (optional)" FORCE)
  endif ()
endif ()
# Set after the i386-mismatch check so OVMF_VARS is already validated.
# Split into two list elements so CMake VERBATIM commands pass them as
# separate arguments rather than a single string with an embedded space.
set(QEMU_UI_OVMF_VARS_ARG "")
if (OVMF_VARS)
  set(QEMU_UI_OVMF_VARS_ARG "--ovmf-vars" "${OVMF_VARS}")
endif ()

if (OVMF_VARS)
  add_custom_target(run-qemu
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_CPU_ARGS} ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
            ${WASMOS_QEMU_NETDEV_ARGS}
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
            -drive format=raw,file=fat:rw:${USERFS_DIR}
            -drive format=raw,file=${WFS_IMAGE}
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
  add_custom_target(run-qemu-debug
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_CPU_ARGS} ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
            -S -gdb tcp::${QEMU_GDB_PORT}
            ${WASMOS_QEMU_NETDEV_ARGS}
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
            -drive format=raw,file=fat:rw:${USERFS_DIR}
            -drive format=raw,file=${WFS_IMAGE}
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU (paused for GDB on port ${QEMU_GDB_PORT})"
    VERBATIM
  )
  add_custom_target(run-qemu-ui
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_CPU_ARGS} ${WASMOS_QEMU_SMP_ARGS} -serial mon:stdio
            ${WASMOS_QEMU_NETDEV_ARGS}
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
            -drive format=raw,file=fat:rw:${USERFS_DIR}
            -drive format=raw,file=${WFS_IMAGE}
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU (with display)"
    VERBATIM
  )
  add_custom_target(run-qemu-test
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_halt_test.py
            --ovmf-code ${OVMF_CODE}
            --ovmf-vars ${OVMF_VARS}
            --esp ${BUILD_DIR}/esp
            --smp ${WASMOS_QEMU_SMP_COUNT}
            --timeout ${WASMOS_QEMU_TEST_TIMEOUT}
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU halt test"
    VERBATIM
  )
  # Standalone SMP scheduler stress gate. Requires a kernel configured with
  # -DWASMOS_SCHED_SMP_STRESS=ON; without it the marker is never emitted, and
  # the script refuses up front (exit 2) rather than reporting the timeout as a
  # stalled ring. Runtime-independent: works on any SMP build and does not
  # require a bootable userspace.
  add_custom_target(run-qemu-sched-stress-test
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_sched_stress_test.py
            --ovmf-code ${OVMF_CODE}
            --ovmf-vars ${OVMF_VARS}
            --esp ${BUILD_DIR}/esp
            --smp ${WASMOS_QEMU_SMP_COUNT}
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running SMP scheduler stress test"
    VERBATIM
  )
  add_custom_target(run-qemu-cli-test
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND ${CMAKE_COMMAND} -E env
            WASMOS_OVMF_CODE=${OVMF_CODE}
            WASMOS_OVMF_VARS=${OVMF_VARS}
            WASMOS_ESP=${BUILD_DIR}/esp
            WASMOS_KERNEL=${BUILD_DIR}/kernel.elf
            WASMOS_USERFS=${USERFS_DIR}
            WASMOS_WFS_IMAGE=${WFS_IMAGE}
            WASMOS_QEMU_ISOLATE_ESP=1
            WASMOS_QEMU_SMP_COUNT=${WASMOS_QEMU_SMP_COUNT}
            PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_unittest_suite.py --start-dir tests
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU CLI integration tests"
    VERBATIM
  )
  add_custom_target(run-qemu-ui-test
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/qemu_ui_test.py
            --ovmf-code ${OVMF_CODE}
            ${QEMU_UI_OVMF_VARS_ARG}
            --esp ${BUILD_DIR}/esp
            --smp ${WASMOS_QEMU_SMP_COUNT}
    DEPENDS bootloader kernel wfs_image
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU UI test"
    VERBATIM
  )
else ()
  if (OVMF_CODE)
    add_custom_target(run-qemu
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_CPU_ARGS} ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
              ${WASMOS_QEMU_NETDEV_ARGS}
              -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
              -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
              -drive format=raw,file=fat:rw:${USERFS_DIR}
              -drive format=raw,file=${WFS_IMAGE}
            -drive format=raw,file=${WFS_IMAGE}
      DEPENDS bootloader kernel wfs_image
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running in QEMU"
      VERBATIM
    )
    add_custom_target(run-qemu-debug
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_CPU_ARGS} ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
              -S -gdb tcp::${QEMU_GDB_PORT}
              ${WASMOS_QEMU_NETDEV_ARGS}
              -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
              -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
              -drive format=raw,file=fat:rw:${USERFS_DIR}
              -drive format=raw,file=${WFS_IMAGE}
            -drive format=raw,file=${WFS_IMAGE}
      DEPENDS bootloader kernel wfs_image
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running in QEMU (paused for GDB on port ${QEMU_GDB_PORT})"
      VERBATIM
    )
    add_custom_target(run-qemu-ui
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_CPU_ARGS} ${WASMOS_QEMU_SMP_ARGS} -serial mon:stdio
              ${WASMOS_QEMU_NETDEV_ARGS}
              -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
              -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
              -drive format=raw,file=fat:rw:${USERFS_DIR}
              -drive format=raw,file=${WFS_IMAGE}
            -drive format=raw,file=${WFS_IMAGE}
      DEPENDS bootloader kernel wfs_image
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running in QEMU (with display)"
      VERBATIM
    )
    add_custom_target(run-qemu-test
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_halt_test.py
              --ovmf-code ${OVMF_CODE}
              --esp ${BUILD_DIR}/esp
              --smp ${WASMOS_QEMU_SMP_COUNT}
      DEPENDS bootloader kernel wfs_image
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running QEMU halt test"
      VERBATIM
    )
    add_custom_target(run-qemu-sched-stress-test
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_sched_stress_test.py
              --ovmf-code ${OVMF_CODE}
              --esp ${BUILD_DIR}/esp
              --smp ${WASMOS_QEMU_SMP_COUNT}
      DEPENDS bootloader kernel wfs_image
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running SMP scheduler stress test"
      VERBATIM
    )
    add_custom_target(run-qemu-cli-test
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND ${CMAKE_COMMAND} -E env
              WASMOS_OVMF_CODE=${OVMF_CODE}
              WASMOS_ESP=${BUILD_DIR}/esp
            WASMOS_KERNEL=${BUILD_DIR}/kernel.elf
              WASMOS_USERFS=${USERFS_DIR}
              WASMOS_WFS_IMAGE=${WFS_IMAGE}
            WASMOS_WFS_IMAGE=${WFS_IMAGE}
              WASMOS_QEMU_ISOLATE_ESP=1
              WASMOS_QEMU_SMP_COUNT=${WASMOS_QEMU_SMP_COUNT}
              PYTHONDONTWRITEBYTECODE=1
              ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_unittest_suite.py --start-dir tests
      DEPENDS bootloader kernel wfs_image
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running QEMU CLI integration tests"
      VERBATIM
    )
  endif ()
endif ()

add_custom_target(run-qemu-ring3-check
  ${WASMOS_ESP_POPULATE_CMDS}
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_ring3_halt_test.py
          --ovmf-code ${OVMF_CODE}
          ${QEMU_UI_OVMF_VARS_ARG}
          --esp ${BUILD_DIR}/esp
  DEPENDS bootloader kernel
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running QEMU ring3 smoke marker test"
  VERBATIM
)

add_custom_target(run-qemu-ring3-threading-check
  ${WASMOS_ESP_POPULATE_CMDS}
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_ring3_halt_test.py
          --ovmf-code ${OVMF_CODE}
          ${QEMU_UI_OVMF_VARS_ARG}
          --esp ${BUILD_DIR}/esp
          --require-threading
  DEPENDS bootloader kernel
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running QEMU ring3 threading lifecycle marker test"
  VERBATIM
)

add_custom_target(run-qemu-ring3-fault-storm-check
  ${WASMOS_ESP_POPULATE_CMDS}
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/scripts/qemu_ring3_fault_storm_test.py
          --ovmf-code ${OVMF_CODE}
          ${QEMU_UI_OVMF_VARS_ARG}
          --esp ${BUILD_DIR}/esp
  DEPENDS bootloader kernel
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running QEMU ring3 multi-process fault-storm marker test"
  VERBATIM
)

set(_ring3_ovmf_vars_arg "")
if (OVMF_VARS)
  set(_ring3_ovmf_vars_arg "--ovmf-vars" "${OVMF_VARS}")
endif ()

# WASMOS_RING3_BUILD_DIR — optional path to an already-configured cmake build
# directory to use for ring3 tests instead of the auto-configured shadow tree.
# When set the ring3 test targets skip configure and build the ring3 check
# target directly in that directory.  The directory must already be configured
# by the caller (e.g. cmake -S . -B my-wasm3-build -DWASMOS_RING3_SMOKE=ON).
set(WASMOS_RING3_BUILD_DIR "" CACHE PATH
    "Pre-configured build directory for ring3 tests (skips auto-configure)")

# Helper: build the common arg list for run_ring3_shadow.py, honouring
# WASMOS_RING3_BUILD_DIR when provided.
macro(_ring3_shadow_cmd _build_subdir _extra_defines _target _out_args)
  if (WASMOS_RING3_BUILD_DIR)
    set(${_out_args}
        ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/run_ring3_shadow.py
        --cmake ${CMAKE_COMMAND}
        --build-dir ${WASMOS_RING3_BUILD_DIR}
        --skip-configure
        --target ${_target})
  else ()
    set(${_out_args}
        ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/run_ring3_shadow.py
        --cmake ${CMAKE_COMMAND}
        --source-dir ${CMAKE_SOURCE_DIR}
        --build-dir ${BUILD_DIR}/${_build_subdir}
        --clang ${CLANG}
        --lld ${LLD}
        --objcopy ${OBJCOPY}
        --ovmf-code ${OVMF_CODE}
        ${_ring3_ovmf_vars_arg}
        --trace ${WASMOS_TRACE}
        ${_extra_defines}
        --target ${_target})
  endif ()
endmacro()

_ring3_shadow_cmd(ring3
  "--define;WASMOS_RING3_SMOKE=ON;--define;WASMOS_PM_TEST_HOOKS=ON"
  run-qemu-ring3-check _ring3_cmd)
add_custom_target(run-qemu-ring3-test
  COMMAND ${_ring3_cmd}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Build ring3 tree and run ring3 smoke marker test"
  VERBATIM
)

_ring3_shadow_cmd(ring3-threading
  "--define;WASMOS_RING3_SMOKE=ON;--define;WASMOS_RING3_THREAD_LIFECYCLE_SMOKE=ON;--define;WASMOS_PM_TEST_HOOKS=ON"
  run-qemu-ring3-threading-check _ring3_threading_cmd)
add_custom_target(run-qemu-ring3-threading-test
  COMMAND ${_ring3_threading_cmd}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Build ring3 tree and run lifecycle marker test"
  VERBATIM
)

_ring3_shadow_cmd(ring3-fault-storm
  "--define;WASMOS_RING3_SMOKE=ON;--define;WASMOS_PM_TEST_HOOKS=ON"
  run-qemu-ring3-fault-storm-check _ring3_fault_storm_cmd)
add_custom_target(run-qemu-ring3-fault-storm-test
  COMMAND ${_ring3_fault_storm_cmd}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Build ring3 tree and run fault-storm marker test"
  VERBATIM
)
add_custom_target(strict-ring3
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target run-qemu-test
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target run-qemu-ring3-test
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Run strict ring3 gate profile (run-qemu-test + run-qemu-ring3-test)"
  VERBATIM
)

add_dependencies(make_initfs c_examples drivers services wamos_script_initfs_data)

set(_wasmos_qemu_targets
  run-qemu run-qemu-ui run-qemu-test run-qemu-cli-test run-qemu-debug
  run-qemu-sched-stress-test
  run-qemu-ring3-check run-qemu-ring3-threading-check run-qemu-ring3-fault-storm-check
)
if (TARGET run-qemu-ui-test)
  list(APPEND _wasmos_qemu_targets run-qemu-ui-test)
endif ()

foreach(_qt IN LISTS _wasmos_qemu_targets)
  if (TARGET ${_qt})
    add_dependencies(${_qt} c_examples services drivers make_initfs)
    if (TARGET system_utils)
      add_dependencies(${_qt} system_utils)
    endif ()
    if (TARGET assemblyscript_examples)
      add_dependencies(${_qt} assemblyscript_examples)
    endif ()
    if (TARGET rust_examples)
      add_dependencies(${_qt} rust_examples)
    endif ()
    if (TARGET go_examples)
      add_dependencies(${_qt} go_examples)
    endif ()
    if (TARGET zig_examples)
      add_dependencies(${_qt} zig_examples)
    endif ()
    if (TARGET zig_calculator)
      add_dependencies(${_qt} zig_calculator)
    endif ()
    if (TARGET ps_util)
      add_dependencies(${_qt} ps_util)
    endif ()
    if (TARGET cat_util)
      add_dependencies(${_qt} cat_util)
    endif ()
  endif ()
endforeach()

# Explicit sysroot for the host sanitizer builds (see the note in the target).
# Sanitizer arms build with the HOST toolchain, not ${CLANG}. On macOS the
# Homebrew LLVM this project uses for the UEFI targets links a TSan runtime that
# segfaults on entry; Apple's clang ships one that works. The kernel sources are
# plain C11 here, so either compiler is fine for the code itself.
set(WASMOS_HOST_SANITIZER_CC ${CLANG})
set(WASMOS_HOST_SANITIZER_SYSROOT "")
if(APPLE)
  find_program(WASMOS_XCRUN xcrun)
  if(WASMOS_XCRUN)
    set(WASMOS_HOST_SANITIZER_CC ${WASMOS_XCRUN} clang)
  endif()
  execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
                  OUTPUT_VARIABLE WASMOS_HOST_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  if(WASMOS_HOST_SDK_PATH)
    set(WASMOS_HOST_SANITIZER_SYSROOT -isysroot ${WASMOS_HOST_SDK_PATH})
  endif()
endif()

# asc and node for the AssemblyScript unit test. ASC_EXECUTABLE is found inside
# examples/assemblyscript, whose scope does not reach here, so resolve them once
# at this level. Both are already build requirements (AS apps; the lint gate's
# QUALITY_ASC), so this introduces no new dependency.
find_program(WASMOS_UNIT_ASC asc HINTS ${CMAKE_SOURCE_DIR}/node_modules/.bin ${CLANG_BIN_DIR})
find_program(WASMOS_UNIT_NODE node)
find_program(WASMOS_UNIT_TINYGO tinygo)

# FAT images from the platform's own formatter (mkfs.vfat / newfs_msdos),
# driven read+write and then checked by fsck.vfat / fsck_msdos and by mounting
# the result on the host. Separate from run-kernel-unit-tests because it needs
# tools that are not everywhere; it exits 77 (skip) when they are absent, which
# this target reports as success.
add_custom_target(run-fat-image-tests
  COMMAND ${CMAKE_COMMAND} -E env
          WASMOS_FAT_IMAGE_TEST_BIN=${BUILD_DIR}/test_fat_image
          WASMOS_FAT_IMAGE_WORK=${BUILD_DIR}/fat-images
          bash -c "bash '${CMAKE_SOURCE_DIR}/scripts/run_fat_image_test.sh'; rc=$?; [ \"$rc\" -eq 77 ] && exit 0; exit \"$rc\"" 
  DEPENDS run-kernel-unit-tests
  COMMENT "Driving fs_fat against real formatter-built FAT images"
  VERBATIM)

add_custom_target(run-kernel-unit-tests
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_list.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          -o ${BUILD_DIR}/test_list
  COMMAND ${BUILD_DIR}/test_list
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_hashmap.c
          ${KERNEL_DIR}/hashmap.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          -o ${BUILD_DIR}/test_hashmap
  COMMAND ${BUILD_DIR}/test_hashmap
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${KERNEL_DIR}/include
          # -iquote routes libc/src/string.c's own "string.h"/"ctype.h" to the
          # libc headers; kernel files use <string.h> so they stay on the kernel
          # header. Provides str_copy/str_copy_bytes (defined in libc string.c).
          -iquote ${LIBC_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_subsystem_registry.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          ${KERNEL_DIR}/subsystem_registry.c
          ${KERNEL_DIR}/hashmap.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_subsystem_registry
  COMMAND ${BUILD_DIR}/test_subsystem_registry
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${CMAKE_SOURCE_DIR}/src/drivers/include
          -I${KERNEL_DIR}/include
          -iquote ${LIBC_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wasmos_exec_format.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          ${KERNEL_DIR}/wasmos_exec_format.c
          ${KERNEL_DIR}/subsystem_registry.c
          ${KERNEL_DIR}/hashmap.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wasmos_exec_format
  COMMAND ${BUILD_DIR}/test_wasmos_exec_format
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${SERVICES_DIR}/device_manager
          -I${LIBSYS_WASM_DIR}/include
          # Route quote-includes ("string.h"/"ctype.h") to our libc headers,
          # which declare strcasecmp; the system <string.h> does not under a
          # strict-C11 host compile (e.g. Linux/glibc CI). -iquote leaves
          # system <angle> includes untouched. The symbol is resolved by the
          # host libc at link.
          -iquote ${LIBC_DIR}/include
          # device_manager_types.h now carries the resolved PCI BAR table, whose
          # types live in the shared driver ABI header.
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_device_manager_rules.c
          ${SERVICES_DIR}/device_manager/device_manager_rules.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_device_manager_rules
  COMMAND ${BUILD_DIR}/test_device_manager_rules
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${SERVICES_DIR}/fs_manager
          ${CMAKE_SOURCE_DIR}/tests/unit/test_fs_manager_path.c
          ${SERVICES_DIR}/fs_manager/fs_manager_path.c
          -o ${BUILD_DIR}/test_fs_manager_path
  COMMAND ${BUILD_DIR}/test_fs_manager_path
  # Listing order for `ls`. Split out of cli.c for the same reason
  # fs_manager_path.c is: no IPC, no console, no globals.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${SERVICES_DIR}/cli
          ${CMAKE_SOURCE_DIR}/tests/unit/test_cli_ls_order.c
          ${SERVICES_DIR}/cli/cli_ls_order.c
          -o ${BUILD_DIR}/test_cli_ls_order
  COMMAND ${BUILD_DIR}/test_cli_ls_order
  # FAT directory scanning over a cluster chain. The test supplies its own block
  # layer (a RAM image served synchronously), so fat_block.c is deliberately NOT
  # linked; fat_util.c is, because fat_geom.c logs through it.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_fat
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          -I${LIBSYS_WASM_DIR}/include
          # Same reason as the device_manager suite above: route quote-includes
          # to our libc headers (strcasecmp), leaving <angle> includes alone.
          -iquote ${LIBC_DIR}/include
          # fat_dir.c uses addr_cast without including the header that defines
          # it, matching every other build of this tree (CFLAGS_KERNEL/EFI and
          # the IDE targets all force-include it).
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_fat_dir.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_dir.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_geom.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_alloc.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_name.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_file.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_util.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_fat_dir
  COMMAND ${BUILD_DIR}/test_fat_dir
  # Same sources, but driven against images built by the PLATFORM's formatter
  # and checked afterwards by its fsck. Built here; run by the separate
  # run-fat-image-tests target, which skips when no formatter is installed.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_fat
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          -I${LIBSYS_WASM_DIR}/include
          -iquote ${LIBC_DIR}/include
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_fat_image.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_dir.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_geom.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_alloc.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_name.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_file.c
          ${DRIVER_WASM_DIR}/fs_fat/fat_util.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_fat_image
  # WFS on-disk format: CRC32C, the seeded metadata checksum, and superblock
  # validation. Pure functions over a byte image, so no block layer is linked
  # and the suite needs no device; the driver's reactor is tested separately.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_format.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          -o ${BUILD_DIR}/test_wfs_format
  COMMAND ${BUILD_DIR}/test_wfs_format
  # The WFS formatter, validated by the reader above: the same wfs_super_parse
  # and wfs_checksum_struct the driver will use, turned on a volume the writer
  # produced. A disagreement between the two fails here rather than waiting for
  # a driver that cannot mount its own filesystem.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_mkfs.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          -o ${BUILD_DIR}/test_wfs_mkfs
  COMMAND ${BUILD_DIR}/test_wfs_mkfs
  # The WFS driver operations as tasks on the SYSTEM coroutine runtime:
  # coroutine_wasm.c and ipc_future_wasm.c are LINKED, not reimplemented, so what
  # these suites exercise is the runtime's own parking and resumption. The device
  # under them is stubs_wfs_block_server.c, a RAM-backed BLOCK server sharing one
  # request-encoding check between both suites.
  #
  # Our libc headers go on -I rather than -iquote, unlike the neighbouring
  # suites: wasmos/libsys.h reaches src/libc/include/string.h through a quoted
  # include, and with the host's <string.h> also in scope the SDK's strcpy
  # fortification macro breaks our declaration. Taking our libc for the angle
  # includes too keeps one set of declarations, and libc/src/string.c is linked
  # so those functions come from our implementation rather than by accident from
  # the host's.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_mount.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_mount.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${LIBSYS_WASM_DIR}/coroutine_wasm.c
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wfs_mount
  COMMAND ${BUILD_DIR}/test_wfs_mount
  # The extent-map walk. Nodes are written into a formatted volume by the test,
  # because mkfs_wfs builds no tree yet: every volume it makes has a root with a
  # single inline extent.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_extent.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_extent.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${LIBSYS_WASM_DIR}/coroutine_wasm.c
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wfs_extent
  COMMAND ${BUILD_DIR}/test_wfs_extent
  # The directory scan. Directory blocks are laid out by the test, because
  # mkfs_wfs builds only an empty root; the extent walk is linked in because a
  # directory's blocks are reached through it like any other file's.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_dir.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_dir.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_extent.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${LIBSYS_WASM_DIR}/coroutine_wasm.c
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wfs_dir
  COMMAND ${BUILD_DIR}/test_wfs_dir
  # End to end: mkfs_wfs populates a volume and the DRIVER reads it back. Every
  # other WFS suite checks one side; this is where the writer and the reader meet
  # on a volume with content, so a disagreement about record packing, an object
  # id, a link count, a checksum seed or an extent surfaces as a driver that
  # cannot find a file the formatter placed.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_populate.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_mount.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_dir.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_extent.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${LIBSYS_WASM_DIR}/coroutine_wasm.c
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wfs_populate
  COMMAND ${BUILD_DIR}/test_wfs_populate
  # The read path: name to bytes. Mostly there to pin the slicing — a read
  # starting mid-block, ending mid-block, spanning several, or running off the
  # end — since each of those is an offset arithmetic that looks right and is not.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_read.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_read.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_mount.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_dir.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_extent.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${LIBSYS_WASM_DIR}/coroutine_wasm.c
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wfs_read
  COMMAND ${BUILD_DIR}/test_wfs_read
  # Path resolution, which is what an open or a stat begins with. Its edge cases
  # are the ones a client hits first: a bare root, redundant separators, dot and
  # dotdot, and a component under a file.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${DRIVER_WASM_DIR}/fs_wfs
          -I${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_path.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_path.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_mount.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_dir.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_extent.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_ops.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
          ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
          ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
          ${LIBSYS_WASM_DIR}/coroutine_wasm.c
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_wfs_path
  COMMAND ${BUILD_DIR}/test_wfs_path
  # Per-line IRQ sharer bookkeeping: pure logic, no arch dependency, so it runs on
  # the host (the x86 IRQ backend itself cannot — it carries inline asm).
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_irq_sharing.c
          ${KERNEL_DIR}/irq_sharing.c
          -o ${BUILD_DIR}/test_irq_sharing
  COMMAND ${BUILD_DIR}/test_irq_sharing
  # MSI vector bookkeeping: same reason it runs on the host — pure logic, with
  # the hardware encoding kept out in arch/x86_64/msi_x86_64.c.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_msi_vectors.c
          ${KERNEL_DIR}/msi_vectors.c
          -o ${BUILD_DIR}/test_msi_vectors
  COMMAND ${BUILD_DIR}/test_msi_vectors
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_xfer_buffer/xfer_buffer.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_xfer_buffer_platform.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          ${KERNEL_DIR}/xfer_buffer/xfer_buffer.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          -o ${BUILD_DIR}/test_xfer_buffer_object
  COMMAND ${BUILD_DIR}/test_xfer_buffer_object
  # Run-queue invariants against the REAL sched_thread.c, with the host standing
  # in for the CPUs (WASMOS_HOST_TEST_SMP swaps cpu_local() for a _Thread_local).
  # Project dirs go on -iquote, not -I, so <string.h> reaches the host libc
  # instead of the kernel's, which has no strstr.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_runqueue.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_runqueue_bias_on
  COMMAND ${BUILD_DIR}/test_sched_runqueue_bias_on
  # Both arms of the caller-CPU-bias switch. It is a compile-time choice, so one
  # build can only ever exercise one branch of sched_wake_thread's last_cpu
  # handling; running the matrix keeps the unselected arm from rotting.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=0
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_runqueue.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_runqueue_bias_off
  COMMAND ${BUILD_DIR}/test_sched_runqueue_bias_off
  # sched_event.c against the REAL file. The in-kernel selftest hand-inlines the
  # wait-list manipulation instead of calling in, so it would pass unchanged if
  # this file were deleted; this links it and stubs only the timer, the thread
  # table and the two scheduler entry points.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_event.c
          ${KERNEL_DIR}/sched_event.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_event
  COMMAND ${BUILD_DIR}/test_sched_event
  # The sanitizer runtimes need an explicit sysroot on macOS: the root
  # CMakeLists deliberately blanks CMAKE_OSX_SYSROOT, and without it the TSan
  # binary links but segfaults on entry before running a single test.
  # Concurrency arm: pthreads stand in for CPUs against the real sched_thread.c.
  # Run at several widths -- the steal scan order, the placement tie rotation and
  # the modulus in both pickers all change shape with the CPU count, and more
  # threads than host cores explores interleavings a 1:1 mapping never reaches.
  # smp=1 covers the degenerate paths (no steal victims, one placement candidate).
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=1
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_smp1
  COMMAND ${BUILD_DIR}/test_sched_concurrency_smp1
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=4
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_smp4
  COMMAND ${BUILD_DIR}/test_sched_concurrency_smp4
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=6
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_smp6
  COMMAND ${BUILD_DIR}/test_sched_concurrency_smp6
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=8
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_smp8
  COMMAND ${BUILD_DIR}/test_sched_concurrency_smp8
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=16
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_smp16
  COMMAND ${BUILD_DIR}/test_sched_concurrency_smp16
  # Sanitizer arms. TSan is the only thing that can see a mixed-atomicity access
  # to the cross-CPU fields; it found four in sched_thread.c the first time it
  # ran. ASan/UBSan cover the out-of-bounds and UB classes the plain build and
  # the arm64 host both miss.
  COMMAND ${WASMOS_HOST_SANITIZER_CC} -std=c11 -Wall -pthread -fsanitize=thread -g -O1
          ${WASMOS_HOST_SANITIZER_SYSROOT}
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=4
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_tsan
  COMMAND ${BUILD_DIR}/test_sched_concurrency_tsan
  COMMAND ${WASMOS_HOST_SANITIZER_CC} -std=c11 -Wall -pthread -fsanitize=address,undefined
          -fno-sanitize-recover=all -g -O1
          ${WASMOS_HOST_SANITIZER_SYSROOT}
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SCHED_CALLER_CPU_BIAS=1 -DWASMOS_TEST_NCPU=8
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
          ${KERNEL_DIR}/sched_thread.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_sched_concurrency_asan
  COMMAND ${BUILD_DIR}/test_sched_concurrency_asan
  # The IPC transport against the REAL ipc.c, linked with the real poll.c and
  # the real sched_event.c so the block/wake handoff is the kernel's own; only
  # the timer, the thread table and the two scheduler entry points are stubbed.
  # WASMOS_IPC_TEST_SEAMS compiles in two setters the kernel does not carry:
  # without them the endpoint-id wrap and the saturating notification counter
  # need ~2^32 operations to reach.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_IPC_TEST_SEAMS=1 -DWASMOS_IDTABLE_TEST_SEAMS=1 -DWASMOS_POLL_TEST_SEAMS=1
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc.c
          ${KERNEL_DIR}/ipc.c
          ${KERNEL_DIR}/idtable.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          ${KERNEL_DIR}/poll.c
          ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_ipc
  COMMAND ${BUILD_DIR}/test_ipc
  # The id-addressed object table, the pattern ipc.c had written out longhand
  # twice. Its id allocation, per-owner quota and release-by-owner are each
  # something a bug has already come from, so they are tested on the component
  # rather than only through its users.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_IDTABLE_TEST_SEAMS=1
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_idtable.c
          ${KERNEL_DIR}/idtable.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_idtable
  COMMAND ${BUILD_DIR}/test_idtable
  # The IPC syscalls, driving x86_syscall_handler with a real syscall_frame_t
  # against the real ipc.c. Reply authenticity and the 32-bit-clean argument
  # rule are enforced only here, and were previously covered only by klog string
  # probes fired from the ring3-smoke image.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_SYSCALL_TEST_SEAMS=1
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_syscall_ipc.c
          ${KERNEL_DIR}/syscall.c
          ${KERNEL_DIR}/ipc.c
          ${KERNEL_DIR}/idtable.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          ${KERNEL_DIR}/poll.c
          ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_syscall_ipc
  COMMAND ${BUILD_DIR}/test_syscall_ipc
  # The libsys service event loop. Header-only static inline over the WASM
  # hostcall imports, and off wasm the import attribute is a no-op, so the test
  # supplies its own imports and the dispatch logic runs unmodified.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -iquote ${LIBSYS_WASM_DIR}/include
          -iquote ${LIBC_DIR}/include
          -iquote ${DRIVER_WASM_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libsys_event_loop.c
          -o ${BUILD_DIR}/test_libsys_event_loop
  COMMAND ${BUILD_DIR}/test_libsys_event_loop
  # The IPC host-call shims, BOTH runtimes in one binary so every scenario runs
  # through each and the results are compared. wasm3's raw-call ABI is stack
  # marshalling and WARP's takes an unused context pointer, so neither needs a
  # live engine; the kernel side is the real ipc.c. Both shim TUs are built here
  # regardless of which runtime this tree is configured for -- comparing them is
  # the entire point, and since the extraction they are standalone.
  COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -Wall -Wno-unused-parameter
          -DWASMOS_HOST_TEST_SMP=1
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -c ${KERNEL_DIR}/warp/link_ipc.cpp -o ${BUILD_DIR}/test_warp_link_ipc.o
  # -Wno-unused-parameter: m3ApiRawFunction's signature carries runtime/_ctx/_mem
  # for every host call whether or not one uses them; that is wasm3's ABI, not
  # our style, and the kernel build does not apply -Wextra here either.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -Wno-unused-parameter
          -DWASMOS_HOST_TEST_SMP=1
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -c ${KERNEL_DIR}/wasm3/link_ipc.c -o ${BUILD_DIR}/test_wasm3_link_ipc.o
  # No explicit -std on this step: it mixes C++ and C sources in one invocation
  # and -std=c++17 is rejected for the C ones. The compiler defaults (gnu++17 /
  # gnu17) cover both.
  COMMAND ${CMAKE_CXX_COMPILER} -Wall -Wno-unused-parameter
          -DWASMOS_HOST_TEST_SMP=1
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -x c++ ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_ipc.cpp
          -x none ${BUILD_DIR}/test_warp_link_ipc.o ${BUILD_DIR}/test_wasm3_link_ipc.o
          -x c ${KERNEL_DIR}/ipc.c ${KERNEL_DIR}/idtable.c ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c ${KERNEL_DIR}/poll.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -x none
          -o ${BUILD_DIR}/test_hostcall_ipc
  COMMAND ${BUILD_DIR}/test_hostcall_ipc
  # The DMA host-call shims, BOTH runtimes in one binary, same reason as the IPC
  # pair above: the guest-visible decision must not depend on which engine runs
  # it. capability.c and policy.c are the real ones, so a denial comes out of the
  # same table a driver spawn writes into; the transfer-buffer layer is stubbed
  # inside the test, which is what lets a scenario report a device address
  # outside the granted window.
  # No explicit -std: this step mixes C++ and C sources in one invocation and
  # -std=c++17 is rejected for the C ones. The compiler defaults (gnu++17 /
  # gnu17) cover both, same as the IPC step above.
  COMMAND ${CMAKE_CXX_COMPILER} -Wall -Wno-unused-parameter
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/tests/unit
          -x c++ ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_dma.cpp
                 ${KERNEL_DIR}/warp/link_dma.cpp
          -x c ${KERNEL_DIR}/wasm3/link_dma.c
               ${KERNEL_DIR}/capability.c ${KERNEL_DIR}/policy.c
               ${KERNEL_DIR}/hostcall_dma.c ${KERNEL_DIR}/hostcall_value.c
               ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c
               ${KERNEL_DIR}/list_array_chunk.c ${KERNEL_DIR}/kmem.c
               ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
               ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
               ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_hostcall_dma
  COMMAND ${BUILD_DIR}/test_hostcall_dma
  # process.c's LIFECYCLE state machine, driven from real pthreads. This is the
  # only suite that links process.c: WASMOS_PROCESS_TEST_SEAMS replaces its six
  # inline-asm sites (two stack switches, sti, the higher-half self-relocation
  # probe, the scheduler-context register capture), the KERNEL_HIGHER_HALF_BASE
  # alias helper and the saved-context rip/rsp validator -- none of which a
  # lifecycle question depends on -- and stubs_process_platform.c supplies the
  # paging/mm/pfa surface a spawn needs. test_sched_concurrency.c has the same
  # pthread-as-CPU harness but STUBS this layer out, which is why the
  # kill-vs-retire panic it could never have caught went unnoticed.
  #
  # Run at several widths for the same reasons as test_sched_concurrency: every
  # CPU but one runs a scheduler loop, so the width changes how many dispatchers
  # can retire threads of one process at once.
  #
  # The sweep stops at 8 on purpose. NCPU=16 is the configuration that FOUND the
  # slot-recycle race -- oversubscribing the host makes the OS preempt inside the
  # dispatch windows and stretch them -- but for that same reason it is the arm
  # whose observable behaviour tracks the runner's spare cores rather than the
  # kernel's behaviour: on a 4-core CI runner it completed 118 of 300 rounds where
  # a 10-core dev box completed all 300. Keep it for investigation
  # (-DWASMOS_TEST_NCPU=16), not as a gate arm.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_PROCESS_TEST_SEAMS=1 -DWASMOS_TEST_NCPU=2
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/tests/unit
          ${CMAKE_SOURCE_DIR}/tests/unit/test_process_lifecycle.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_process_platform.c
          ${KERNEL_DIR}/process.c ${KERNEL_DIR}/thread.c ${KERNEL_DIR}/sched_thread.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c ${KERNEL_DIR}/idtable.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/futex.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          -o ${BUILD_DIR}/test_process_lifecycle_smp2
  COMMAND ${BUILD_DIR}/test_process_lifecycle_smp2
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_PROCESS_TEST_SEAMS=1 -DWASMOS_TEST_NCPU=4
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/tests/unit
          ${CMAKE_SOURCE_DIR}/tests/unit/test_process_lifecycle.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_process_platform.c
          ${KERNEL_DIR}/process.c ${KERNEL_DIR}/thread.c ${KERNEL_DIR}/sched_thread.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c ${KERNEL_DIR}/idtable.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/futex.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          -o ${BUILD_DIR}/test_process_lifecycle_smp4
  COMMAND ${BUILD_DIR}/test_process_lifecycle_smp4
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_PROCESS_TEST_SEAMS=1 -DWASMOS_TEST_NCPU=8
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR} -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          -iquote ${DRIVER_WASM_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/tests/unit
          ${CMAKE_SOURCE_DIR}/tests/unit/test_process_lifecycle.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_process_platform.c
          ${KERNEL_DIR}/process.c ${KERNEL_DIR}/thread.c ${KERNEL_DIR}/sched_thread.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c ${KERNEL_DIR}/idtable.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/futex.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          -o ${BUILD_DIR}/test_process_lifecycle_smp8
  COMMAND ${BUILD_DIR}/test_process_lifecycle_smp8
  # The wasm32 struct sizes src/libc/go/coroutine.go hardcodes as opaque
  # [N]uint32 blobs. Go cannot express the C layout, so nothing checks these at
  # the language level and an undersized blob is heap corruption rather than a
  # compile error -- Coroutine was wrong by one word. Compiled for wasm32 and
  # never linked; the static assertions are the test.
  COMMAND ${CLANG} --target=wasm32 -nostdlib -fsyntax-only
          -I${LIBSYS_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_go_abi_sizes.c
  # The AssemblyScript coroutine runtime. AS is the one guest language that
  # cannot link src/libsys/wasm/coroutine_wasm.c (asc has no external linking),
  # so it has a port instead, and this holds the port to the C runtime's
  # behaviour: the cases mirror tests/unit/test_wasm_coroutine.c.
  #
  # asc compiles the test to wasm and node runs it -- there is no host build of
  # AS. Both are already required by the build (AS apps, and the asc found for
  # the lint gate), so this adds no new dependency.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/as_coroutine_test_src
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${LIBC_DIR}/assemblyscript/coroutine.ts ${BUILD_DIR}/as_coroutine_test_src/coroutine.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/as/shuffle.ts
          ${BUILD_DIR}/as_coroutine_test_src/shuffle.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/test_as_coroutine.ts
          ${BUILD_DIR}/as_coroutine_test_src/test_as_coroutine.ts
  COMMAND ${WASMOS_UNIT_ASC} ${BUILD_DIR}/as_coroutine_test_src/test_as_coroutine.ts
          --target release -O --runtime stub
          --outFile ${BUILD_DIR}/test_as_coroutine.wasm
  COMMAND ${WASMOS_UNIT_NODE} ${CMAKE_SOURCE_DIR}/tests/unit/as/run_as_test.mjs
          ${BUILD_DIR}/test_as_coroutine.wasm test_as_coroutine
  # The AssemblyScript event loop and its IPC-future bridge, against the fake
  # IPC fabric in the harness. asc resolves imports relative to the entry and
  # has no include path, so the test is staged flat beside the modules it
  # imports -- the same staging the real AS build does.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/as_eventloop_test_src
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${LIBC_DIR}/assemblyscript/coroutine.ts ${BUILD_DIR}/as_eventloop_test_src/coroutine.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${LIBC_DIR}/assemblyscript/eventloop.ts ${BUILD_DIR}/as_eventloop_test_src/eventloop.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/abi/generated/assemblyscript/wasmos_imports.ts
          ${BUILD_DIR}/as_eventloop_test_src/wasmos_imports.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/as/shuffle.ts
          ${BUILD_DIR}/as_eventloop_test_src/shuffle.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/test_as_eventloop.ts
          ${BUILD_DIR}/as_eventloop_test_src/test_as_eventloop.ts
  COMMAND ${WASMOS_UNIT_ASC} ${BUILD_DIR}/as_eventloop_test_src/test_as_eventloop.ts
          --target release -O --runtime stub
          --outFile ${BUILD_DIR}/test_as_eventloop.wasm
  COMMAND ${WASMOS_UNIT_NODE} ${CMAKE_SOURCE_DIR}/tests/unit/as/run_as_test.mjs
          ${BUILD_DIR}/test_as_eventloop.wasm test_as_eventloop
  # The @coroutine lowering in tools/as_coroutine_transform.mjs. asc has no
  # async/await, so this transform generates the pc state machine a stackless
  # coroutine needs; the test holds the generated machine to the behaviour of
  # the same logic hand-written as a Task.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/as_transform_test_src
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${LIBC_DIR}/assemblyscript/coroutine.ts
          ${BUILD_DIR}/as_transform_test_src/coroutine.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/as/shuffle.ts
          ${BUILD_DIR}/as_transform_test_src/shuffle.ts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/test_as_coroutine_transform.ts
          ${BUILD_DIR}/as_transform_test_src/test_as_coroutine_transform.ts
  COMMAND ${WASMOS_UNIT_ASC} ${BUILD_DIR}/as_transform_test_src/test_as_coroutine_transform.ts
          --target release -O --runtime stub
          --transform ${CMAKE_SOURCE_DIR}/tools/as_coroutine_transform.mjs
          --outFile ${BUILD_DIR}/test_as_coroutine_transform.wasm
  COMMAND ${WASMOS_UNIT_NODE} ${CMAKE_SOURCE_DIR}/tests/unit/as/run_as_test.mjs
          ${BUILD_DIR}/test_as_coroutine_transform.wasm test_as_coroutine_transform
  VERBATIM)

# Its own command, and guarded: the comment below always claimed the Go test is
# skipped when TinyGo is absent, but nothing enforced that, so a machine without
# TinyGo failed the whole suite with a bare "no such file or directory" -- which
# is exactly how CI broke. CI installs TinyGo, so it still runs there.
if (WASMOS_UNIT_TINYGO)
  add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
  # The Go binding, on wasm32. Unlike the Rust and Zig suites -- which run on
  # the host against coroutine_wasm.c, valid because their layouts are computed
  # per target -- Go models every record as an opaque blob sized for wasm32, so
  # a host test would exercise a different ABI. TinyGo builds it against the
  # same target JSON the Go examples use (which already compiles
  # coroutine_wasm.c in), and node runs it. Skipped when TinyGo is absent, the
  # same condition that disables the Go examples.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/go_coroutine_test_src
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/go/coroutine.go
          ${BUILD_DIR}/go_coroutine_test_src/coroutine.go
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/go/wasmos.go
          ${BUILD_DIR}/go_coroutine_test_src/wasmos.go
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/tests/unit/test_wasm_coroutine.go
          ${BUILD_DIR}/go_coroutine_test_src/test_wasm_coroutine.go
  # The target JSON comes from wasmos-tinygo, which is the single definition of
  # what a WASMOS Go build is -- its `extra-files` are relative to TINYGOROOT, so
  # it cannot be a checked-in template and has to be generated per invocation.
  COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-tinygo
          --emit-target-json=${BUILD_DIR}/wasmos-tinygo.json
  # tinygo refuses an absolute package path, and this target's working directory
  # is the source root, so run it from the build directory.
  COMMAND ${CMAKE_COMMAND} -E chdir ${BUILD_DIR}
          ${CMAKE_COMMAND} -E env GO111MODULE=off ${WASMOS_UNIT_TINYGO}
          build -target=${BUILD_DIR}/wasmos-tinygo.json
          -opt=z -no-debug -panic=trap -scheduler=none -gc=leaking
          -o ${BUILD_DIR}/test_wasm_coroutine_go.wasm
          ./go_coroutine_test_src
  COMMAND ${WASMOS_UNIT_NODE} ${CMAKE_SOURCE_DIR}/tests/unit/go/run_go_test.mjs
          ${BUILD_DIR}/test_wasm_coroutine_go.wasm test_wasm_coroutine_go
  VERBATIM)
  # The target JSON names the sysroot's include directory, and the build reads
  # headers from it, so the sysroot has to be staged first.
  add_dependencies(run-kernel-unit-tests wasmos-sdk)
endif ()

add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
  # Block-buffer bounds arithmetic. Pure arithmetic against the real file, no
  # stubs: the guest supplies (offset, len) and the result decides whether a
  # memcpy stays inside the process's 8 KiB buffer.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_block_buffer.c
          ${KERNEL_DIR}/block_buffer.c
          -o ${BUILD_DIR}/test_block_buffer
  COMMAND ${BUILD_DIR}/test_block_buffer
  # The value-return rule: a success value with bit 31 set reads as an error,
  # because the ABI carries both on one signed i32.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_value.c
          ${KERNEL_DIR}/hostcall_value.c
          -o ${BUILD_DIR}/test_hostcall_value
  COMMAND ${BUILD_DIR}/test_hostcall_value
  # The kernel environment store, shared by both runtimes.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_kenv.c
          ${KERNEL_DIR}/kenv.c
          -o ${BUILD_DIR}/test_kenv
  COMMAND ${BUILD_DIR}/test_kenv
  # Filling a caller-supplied name buffer: the clamp, and which length is
  # reported so truncation stays detectable.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_buffer.c
          ${KERNEL_DIR}/hostcall_buffer.c
          -o ${BUILD_DIR}/test_hostcall_buffer
  COMMAND ${BUILD_DIR}/test_hostcall_buffer
  # The poll hub on its own. Its only outward call is ipc_select_signal, so the
  # test stubs that and asserts exactly which sets a notify reaches.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_poll.c
          ${KERNEL_DIR}/poll.c
          -o ${BUILD_DIR}/test_poll
  COMMAND ${BUILD_DIR}/test_poll
  # The futex, against the real file. The mm stub places its fake physical base
  # so paddr + KERNEL_HIGHER_HALF_BASE lands on a host buffer, which is exactly
  # the translation futex_wait performs. WASMOS_FUTEX_TEST_SEAMS adds the table
  # teardown the kernel deliberately lacks (entries live for the system's life).
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_FUTEX_TEST_SEAMS=1
          -include ${LIBC_DIR}/include/wasmos_cast.h
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_futex.c
          ${KERNEL_DIR}/futex.c
          ${KERNEL_DIR}/sched_event.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_futex
  COMMAND ${BUILD_DIR}/test_futex
  # IPC concurrency: pthreads stand in for CPUs against the real ipc.c. Two
  # widths, because the contention shape changes with the sender/receiver split
  # and more threads than host cores explores interleavings a 1:1 mapping never
  # reaches.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_TEST_NTHREADS=4
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc_concurrency.c
          ${KERNEL_DIR}/ipc.c ${KERNEL_DIR}/idtable.c ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          ${KERNEL_DIR}/poll.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_ipc_concurrency_t4
  COMMAND ${BUILD_DIR}/test_ipc_concurrency_t4
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions -pthread
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_TEST_NTHREADS=8
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc_concurrency.c
          ${KERNEL_DIR}/ipc.c ${KERNEL_DIR}/idtable.c ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          ${KERNEL_DIR}/poll.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_ipc_concurrency_t8
  COMMAND ${BUILD_DIR}/test_ipc_concurrency_t8
  # Sanitizer arms. TSan is the only thing that can see ready_ep being read
  # under the wrong lock -- it found exactly that the first time it ran. ASan
  # covers the poll_struct use-after-free class around endpoint teardown, which
  # a plain build reports as nothing at all.
  COMMAND ${WASMOS_HOST_SANITIZER_CC} -std=c11 -Wall -pthread -fsanitize=thread -g -O1
          ${WASMOS_HOST_SANITIZER_SYSROOT}
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_TEST_NTHREADS=4
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc_concurrency.c
          ${KERNEL_DIR}/ipc.c ${KERNEL_DIR}/idtable.c ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          ${KERNEL_DIR}/poll.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_ipc_concurrency_tsan
  COMMAND ${BUILD_DIR}/test_ipc_concurrency_tsan
  COMMAND ${WASMOS_HOST_SANITIZER_CC} -std=c11 -Wall -pthread
          -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1
          ${WASMOS_HOST_SANITIZER_SYSROOT}
          -DWASMOS_HOST_TEST_SMP=1 -DWASMOS_TEST_NTHREADS=8
          -iquote ${KERNEL_DIR}/include
          -iquote ${CMAKE_SOURCE_DIR}/libs/wasm3/source
          -iquote ${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc_concurrency.c
          ${KERNEL_DIR}/ipc.c ${KERNEL_DIR}/idtable.c ${CMAKE_SOURCE_DIR}/tests/unit/stubs_kpanic.c
          ${KERNEL_DIR}/poll.c ${KERNEL_DIR}/sched_event.c
          ${KERNEL_DIR}/list.c ${KERNEL_DIR}/list_linked.c ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_spinlock.c
          -o ${BUILD_DIR}/test_ipc_concurrency_asan
  COMMAND ${BUILD_DIR}/test_ipc_concurrency_asan
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_warp_driver_ring3_call_policy.c
          ${KERNEL_DIR}/warp_driver_ring3_call_policy.c
          -o ${BUILD_DIR}/test_warp_driver_ring3_call_policy
  COMMAND ${BUILD_DIR}/test_warp_driver_ring3_call_policy
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -DWASMOS_TEST_USE_REAL_SPINLOCK_DECLS
          -pthread
          -I${CMAKE_SOURCE_DIR}/tests/unit/include
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_kernel_sync_primitives.c
          ${KERNEL_DIR}/sync_mutex.c
          ${KERNEL_DIR}/sync_semaphore.c
          -o ${BUILD_DIR}/test_kernel_sync_primitives
  COMMAND ${BUILD_DIR}/test_kernel_sync_primitives
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_user_mutex.c
          -o ${BUILD_DIR}/test_user_mutex
  COMMAND ${BUILD_DIR}/test_user_mutex
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_stdio.c
          ${LIBC_DIR}/src/stdio.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_libc_stdio
  COMMAND ${BUILD_DIR}/test_libc_stdio
  # sqrtf convergence across the exponent range. math.c only, no stubs: these
  # are pure functions, and the reference is the defining property (got*got == x)
  # rather than the host libm, which this TU cannot reach anyway -- the project
  # <math.h> is on -I and the two libcs cannot both be in scope.
  #
  # -fno-builtin is load-bearing, not hygiene: clang knows sqrtf and constant-
  # folds every call with a literal argument to the correct IEEE result, so
  # without it the cases never reach math.c and the suite passes against an
  # arbitrarily broken implementation. It was written without the flag first,
  # and both the old and the fixed sqrtf produced identical output.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -fno-builtin
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_math.c
          ${LIBC_DIR}/src/math.c
          -o ${BUILD_DIR}/test_libc_math
  COMMAND ${BUILD_DIR}/test_libc_math
  # The libui Zig shim's arena allocator. Compiled for wasm32 in the real build
  # and linked only into the Zig calculator, but it is plain freestanding C over
  # a static buffer, so the host runs it unchanged. -fno-builtin keeps clang from
  # recognising malloc/calloc/realloc and reasoning about them instead of
  # calling these. WASMOS_LIBUI_SHIM_TEST_SEAMS exposes an arena reset so each
  # case starts clean and the suite can be shuffled; the guest build never
  # defines it, and the arena still never reclaims there.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -fno-builtin
          -DWASMOS_LIBUI_SHIM_TEST_SEAMS=1
          -I${LIBC_DIR}/include
          -I${CMAKE_SOURCE_DIR}/src/libui/include
          -I${DRIVER_WASM_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libui_shim_alloc.c
          ${CMAKE_SOURCE_DIR}/src/libui/zig/libui_shim.c
          -o ${BUILD_DIR}/test_libui_shim_alloc
  COMMAND ${BUILD_DIR}/test_libui_shim_alloc
  # memcmp's argument guards and comparison sign. -fno-builtin for the same
  # reason as the math suite above: memcmp is a clang builtin and a folded call
  # never reaches string.c.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -fno-builtin
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_string.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_libc_string
  COMMAND ${BUILD_DIR}/test_libc_string
  # argv construction from the spawn-info argument string. spawn_info.c reaches
  # the contract through two hostcalls, which the suite defines itself, so it
  # links against the real accessors with a synthetic spawn-info blob behind them.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/tests/unit
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_startup_argv.c
          ${LIBC_DIR}/src/spawn_info.c
          -o ${BUILD_DIR}/test_libc_startup_argv
  COMMAND ${BUILD_DIR}/test_libc_startup_argv
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -Wno-error=implicit-function-declaration -Wno-sign-compare
          "-D__builtin_wasm_memory_size(x)=((int)(0))"
          "-D__builtin_wasm_memory_grow(x,y)=((int)(-1))"
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          -include ${LIBC_DIR}/include/wasmos_cast.h
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_stdlib.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wasm_stdlib.c
          ${LIBC_DIR}/src/stdlib.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_libc_stdlib
  COMMAND ${BUILD_DIR}/test_libc_stdlib
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${LIBSYS_WASM_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_vring.c
          -o ${BUILD_DIR}/test_vring
  COMMAND ${BUILD_DIR}/test_vring
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -pthread
          -I${LIBSYS_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/src/drivers/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_ringbuf.c
          -o ${BUILD_DIR}/test_ringbuf
  COMMAND ${BUILD_DIR}/test_ringbuf
  # libui GFX_EVENT_KEY decoding: the compositor packs scancode:character into
  # one code, so the handlers must mask before treating it as a character.
  # Header-only widgets, so the test compiles them straight into the host binary.
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/src/libui/include
          -I${LIBC_DIR}/include
          -I${CMAKE_SOURCE_DIR}/src/drivers/include
          -I${CMAKE_SOURCE_DIR}/abi/generated/c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libui_key_decode.c
          -o ${BUILD_DIR}/test_libui_key_decode
  COMMAND ${BUILD_DIR}/test_libui_key_decode
  # Native slab allocator (heap_native.c): compiled with malloc/free/calloc/realloc
  # renamed to hn_* so it links beside host libc; vm_map/vm_unmap mocked in the test.
  COMMAND ${CLANG} -std=c11 -Wno-error=c23-extensions
          -Dmalloc=hn_malloc -Dfree=hn_free -Dcalloc=hn_calloc -Drealloc=hn_realloc
          -I${CMAKE_SOURCE_DIR}/src/drivers/include
          -c ${CMAKE_SOURCE_DIR}/src/libsys/native/heap_native.c
          -o ${BUILD_DIR}/test_heap_native_alloc.o
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/src/drivers/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_heap_native.c
          ${BUILD_DIR}/test_heap_native_alloc.o
          -o ${BUILD_DIR}/test_heap_native
  COMMAND ${BUILD_DIR}/test_heap_native
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${LIBSYS_WASM_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          -I${CMAKE_SOURCE_DIR}/src/services/net_stack
          ${CMAKE_SOURCE_DIR}/tests/unit/test_net_socket.c
          ${CMAKE_SOURCE_DIR}/src/services/net_stack/socket.c
          -o ${BUILD_DIR}/test_net_socket
  COMMAND ${BUILD_DIR}/test_net_socket
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${CMAKE_SOURCE_DIR}/src/services/net_stack
          ${CMAKE_SOURCE_DIR}/tests/unit/test_net_ifcfg.c
          ${CMAKE_SOURCE_DIR}/src/services/net_stack/net_stack_ifcfg.c
          -o ${BUILD_DIR}/test_net_ifcfg
  COMMAND ${BUILD_DIR}/test_net_ifcfg
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          -iquote ${LIBC_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_service_class_registry.c
          ${KERNEL_DIR}/service_class_registry.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          ${KERNEL_DIR}/kmem.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_slab.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_service_class_registry
  COMMAND ${BUILD_DIR}/test_service_class_registry
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running host kernel unit tests"
  VERBATIM
)

if(TARGET warp_wasm_coroutine_test)
  add_dependencies(run-kernel-unit-tests warp_wasm_coroutine_test)
endif()

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(NATIVE_COROUTINE_HOST_ASM ${LIBSYS_NATIVE_DIR}/coroutine_native_x86_64.S)
  set(NATIVE_COROUTINE_HOST_LABEL "x86_64")
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(NATIVE_COROUTINE_HOST_ASM ${LIBSYS_NATIVE_DIR}/coroutine_native_aarch64.S)
  set(NATIVE_COROUTINE_HOST_LABEL "aarch64")
endif()

set(NATIVE_COROUTINE_ZIG_TARGET_ARGS)
set(NATIVE_COROUTINE_ZIG_LINK_ARGS)
if(APPLE AND DEFINED NATIVE_COROUTINE_HOST_ASM)
  # Pin the deployment target so Zig links the host SDK's libSystem rather
  # than requesting APIs newer than the installed SDK exports.
  list(APPEND NATIVE_COROUTINE_ZIG_TARGET_ARGS
       -target ${NATIVE_COROUTINE_HOST_LABEL}-macos.15.0)
  list(APPEND NATIVE_COROUTINE_ZIG_LINK_ARGS -lSystem)
endif()

if(DEFINED NATIVE_COROUTINE_HOST_ASM)
  if (RUST_ENABLE)
    find_program(RUST_COROUTINE_TEST_RUSTC rustc)
  endif ()
  if (RUST_COROUTINE_TEST_RUSTC)
    add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
      COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
              -I${LIBSYS_WASM_DIR}/include
              -c ${LIBSYS_WASM_DIR}/coroutine_wasm.c
              -o ${BUILD_DIR}/test_wasm_coroutine_rust_core.o
      COMMAND ${RUST_COROUTINE_TEST_RUSTC} --test ${CMAKE_SOURCE_DIR}/tests/unit/test_wasm_coroutine.rs
              -C link-arg=${BUILD_DIR}/test_wasm_coroutine_rust_core.o
              -o ${BUILD_DIR}/test_wasm_coroutine_rust
      # --test-threads=1 is REQUIRED, not tidiness. coroutine_wasm.c keeps the
      # active runtime in a file-global (g_current_runtime), which is sound for
      # its target -- a WASM guest is single-threaded and has one cooperative
      # runtime per module -- but Rust's harness runs #[test] cases on parallel
      # threads by default, so two cases racing on that global made a waiter
      # park against the other case's runtime and never wake. Intermittent at
      # roughly 1 run in 10; every other language's suite is single-threaded and
      # never saw it.
      COMMAND ${BUILD_DIR}/test_wasm_coroutine_rust --test-threads=1
      COMMENT "Running Rust WASM coroutine wrapper unit tests"
      VERBATIM)
  endif ()
  if (ZIG_AVAILABLE)
    add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
      COMMAND ${ZIG_EXECUTABLE} test
              ${NATIVE_COROUTINE_ZIG_TARGET_ARGS}
              ${LIBSYS_WASM_DIR}/coroutine_wasm.c
              -I${LIBSYS_WASM_DIR}/include
              --cache-dir ${BUILD_DIR}/zig-wasm-coroutine-cache
              --global-cache-dir ${BUILD_DIR}/zig-wasm-coroutine-global-cache
              --dep coroutine
              -Mroot=${CMAKE_SOURCE_DIR}/tests/unit/test_wasm_coroutine.zig
              -Mcoroutine=${LIBC_DIR}/zig/coroutine.zig
              ${NATIVE_COROUTINE_ZIG_LINK_ARGS}
      COMMENT "Running WASM Zig coroutine wrapper unit tests"
      VERBATIM)
  endif ()
  add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
    COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
            -I${LIBSYS_WASM_DIR}/include
            -I${LIBC_DIR}/include
            -I${DRIVER_WASM_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests/unit/test_wasm_coroutine.c
            ${LIBSYS_WASM_DIR}/coroutine_wasm.c
            ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
            -o ${BUILD_DIR}/test_wasm_coroutine
    COMMAND ${BUILD_DIR}/test_wasm_coroutine
    COMMENT "Running host WASM stackless coroutine unit tests"
    VERBATIM)
  add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
    COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
            -I${LIBSYS_NATIVE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests/unit/test_native_coroutine.c
            ${LIBSYS_NATIVE_DIR}/coroutine_native.c
            ${NATIVE_COROUTINE_HOST_ASM}
            -o ${BUILD_DIR}/test_native_coroutine
    COMMAND ${BUILD_DIR}/test_native_coroutine
    COMMENT "Running native ${NATIVE_COROUTINE_HOST_LABEL} coroutine unit tests"
    VERBATIM)
  add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
    COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
            -I${LIBSYS_NATIVE_DIR}/include
            -I${DRIVER_WASM_DIR}/include
            -I${LIBC_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests/unit/test_native_ipc_future.c
            ${CMAKE_SOURCE_DIR}/tests/unit/stubs_native_libsys.c
            ${LIBSYS_NATIVE_DIR}/coroutine_native.c
            ${LIBSYS_NATIVE_DIR}/ipc_future_native.c
            ${LIBSYS_NATIVE_DIR}/service_runtime_native.c
            ${LIBSYS_NATIVE_DIR}/zig/libsys_native.c
            ${NATIVE_COROUTINE_HOST_ASM}
            -o ${BUILD_DIR}/test_native_ipc_future
    COMMAND ${BUILD_DIR}/test_native_ipc_future
    COMMENT "Running native ${NATIVE_COROUTINE_HOST_LABEL} IPC future unit tests"
    VERBATIM)
  if (ZIG_AVAILABLE)
    add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
      COMMAND ${ZIG_EXECUTABLE} test
              ${NATIVE_COROUTINE_ZIG_TARGET_ARGS}
              ${LIBSYS_NATIVE_DIR}/coroutine_native.c
              ${LIBSYS_NATIVE_DIR}/ipc_future_native.c
              ${LIBSYS_NATIVE_DIR}/service_runtime_native.c
              ${LIBSYS_NATIVE_DIR}/zig/libsys_native.c
              ${CMAKE_SOURCE_DIR}/tests/unit/stubs_native_libsys.c
              ${NATIVE_COROUTINE_HOST_ASM}
              -I${LIBSYS_NATIVE_DIR}/include
              -I${CMAKE_SOURCE_DIR}/src/drivers/include
              -I${LIBC_DIR}/include
              --cache-dir ${BUILD_DIR}/zig-native-coroutine-cache
              --global-cache-dir ${BUILD_DIR}/zig-global-cache
              --dep libsys
              -Mroot=${CMAKE_SOURCE_DIR}/tests/unit/test_native_coroutine.zig
              -Mlibsys=${LIBSYS_NATIVE_DIR}/zig/libsys.zig
              -I${LIBSYS_NATIVE_DIR}/include
              -I${CMAKE_SOURCE_DIR}/src/drivers/include
              ${NATIVE_COROUTINE_ZIG_LINK_ARGS}
      COMMENT "Running native ${NATIVE_COROUTINE_HOST_LABEL} Zig coroutine wrapper unit tests"
      VERBATIM)
  endif ()
else()
  add_custom_command(TARGET run-kernel-unit-tests POST_BUILD
    COMMAND ${CLANG} -target ${KERNEL_TARGET_TRIPLE} -std=c11 -ffreestanding
            -fno-stack-protector -fno-pic -fno-pie
            -I${LIBSYS_NATIVE_DIR}/include
            -c ${LIBSYS_NATIVE_DIR}/coroutine_native.c
            -o ${BUILD_DIR}/test_native_coroutine_target.o
    COMMAND ${CLANG} -target ${KERNEL_TARGET_TRIPLE}
            -c ${LIBSYS_NATIVE_DIR}/coroutine_native_x86_64.S
            -o ${BUILD_DIR}/test_native_coroutine_switch_target.o
    COMMENT "Cross-compiling native x86_64 coroutine runtime"
    VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Developer quality targets: formatting and linting.
#
# scripts/quality.sh is the backing implementation (it owns the source
# allowlist and per-language tool invocations). CMake has already resolved the
# toolchain, so rather than have the script re-search PATH, pass the paths we
# know through the env-var overrides it honors. Tools CMake cannot locate here
# are left unset; the script then falls back to PATH and only errors if a
# language with matching in-scope sources is actually present.
# CMAKE_EXPORT_COMPILE_COMMANDS (set at the top of this file) gives clang-tidy
# its compile database under the build dir.
# ---------------------------------------------------------------------------
# An exported CLANG_FORMAT wins over the search. The fmt/lint targets run the
# script through `cmake -E env CLANG_FORMAT=...`, so whatever is found here
# REPLACES the caller's environment rather than defaulting to it -- which meant
# a pinned clang-format exported by CI was overridden by the toolchain copy
# find_program happened to locate, and the version guard then reported the pin
# as wrong. Searching remains the behaviour when the variable is unset.
if (DEFINED ENV{CLANG_FORMAT} AND NOT QUALITY_CLANG_FORMAT)
  set(QUALITY_CLANG_FORMAT "$ENV{CLANG_FORMAT}" CACHE FILEPATH "clang-format for the quality gate")
endif ()
find_program(QUALITY_CLANG_FORMAT clang-format HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS})
find_program(QUALITY_CLANG_TIDY clang-tidy HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS})
find_program(QUALITY_ZIG zig)
find_program(QUALITY_GOFMT gofmt)
find_program(QUALITY_GO go)
find_program(QUALITY_RUSTFMT rustfmt)
find_program(QUALITY_RUSTC rustc)
find_program(QUALITY_ASC asc HINTS ${CMAKE_SOURCE_DIR}/node_modules/.bin)
find_program(QUALITY_BLACK black)
find_program(QUALITY_RUFF ruff)

# wasmos clang-tidy plugin (tools/clang_tidy_wasmos): compiled here against the
# resolved LLVM so the loaded module's ABI matches clang-tidy exactly, then
# loaded via --load in run_clang_tidy. Rebuilt only when the source changes.
# Requires llvm-config + clang-tidy's dev headers; if unavailable the plugin is
# skipped and the wasmos-* checks simply do not run.
#
# WASMOS_CLANG_TIDY_INCLUDE_DIR: extra include dir providing clang-tidy/*.h for
# platforms that ship the clang-tidy binary but not its headers (e.g. Ubuntu's
# apt.llvm.org packages). Point it at a clang-tools-extra checkout of the
# matching LLVM version; leave empty where the headers sit alongside the clang
# headers already (Homebrew's llvm).
set(WASMOS_CLANG_TIDY_INCLUDE_DIR "" CACHE PATH
    "Extra include dir providing clang-tidy/*.h for the wasmos clang-tidy plugin")
find_program(LLVM_CONFIG llvm-config HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS})
set(WASMOS_TIDY_PLUGIN "")
if (QUALITY_CLANG_TIDY AND LLVM_CONFIG AND CLANGXX_PATH AND
    EXISTS ${CMAKE_SOURCE_DIR}/tools/clang_tidy_wasmos/WasmosTidyModule.cpp)
  set(WASMOS_TIDY_PLUGIN_SRC ${CMAKE_SOURCE_DIR}/tools/clang_tidy_wasmos/WasmosTidyModule.cpp)
  set(WASMOS_TIDY_PLUGIN ${CMAKE_BINARY_DIR}/wasmos_tidy_plugin${CMAKE_SHARED_LIBRARY_SUFFIX})
  execute_process(COMMAND ${LLVM_CONFIG} --cxxflags
    OUTPUT_VARIABLE _llvm_cxxflags OUTPUT_STRIP_TRAILING_WHITESPACE)
  separate_arguments(_llvm_cxxflags_list UNIX_COMMAND "${_llvm_cxxflags}")
  set(_tidy_extra_inc "")
  if (WASMOS_CLANG_TIDY_INCLUDE_DIR)
    set(_tidy_extra_inc -I${WASMOS_CLANG_TIDY_INCLUDE_DIR})
  endif ()
  if (APPLE)
    set(_tidy_plugin_link -undefined dynamic_lookup)
  else ()
    set(_tidy_plugin_link -Wl,--allow-shlib-undefined)
  endif ()
  add_custom_command(
    OUTPUT ${WASMOS_TIDY_PLUGIN}
    COMMAND ${CLANGXX_PATH} ${_llvm_cxxflags_list} ${_tidy_extra_inc} -fPIC -shared
            ${_tidy_plugin_link} ${WASMOS_TIDY_PLUGIN_SRC} -o ${WASMOS_TIDY_PLUGIN}
    DEPENDS ${WASMOS_TIDY_PLUGIN_SRC}
    COMMENT "Building wasmos clang-tidy plugin"
    VERBATIM)
  add_custom_target(wasmos_tidy_plugin DEPENDS ${WASMOS_TIDY_PLUGIN})
endif ()

set(QUALITY_ENV QUALITY_BUILD_DIR=${CMAKE_BINARY_DIR})
if (WASMOS_TIDY_PLUGIN)
  list(APPEND QUALITY_ENV QUALITY_TIDY_PLUGIN=${WASMOS_TIDY_PLUGIN})
endif ()
foreach (_pair
    CLANG_FORMAT:QUALITY_CLANG_FORMAT
    CLANG_TIDY:QUALITY_CLANG_TIDY
    ZIG:QUALITY_ZIG
    GOFMT:QUALITY_GOFMT
    GO:QUALITY_GO
    RUSTFMT:QUALITY_RUSTFMT
    RUSTC:QUALITY_RUSTC
    ASC:QUALITY_ASC
    BLACK:QUALITY_BLACK
    RUFF:QUALITY_RUFF)
  string(REPLACE ":" ";" _kv ${_pair})
  list(GET _kv 0 _env_name)
  list(GET _kv 1 _var_name)
  if (${_var_name})
    list(APPEND QUALITY_ENV ${_env_name}=${${_var_name}})
  endif ()
endforeach ()

set(QUALITY_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/quality.sh)

add_custom_target(fmt
  COMMAND ${CMAKE_COMMAND} -E env ${QUALITY_ENV} bash ${QUALITY_SCRIPT} format
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL VERBATIM
  COMMENT "quality: format in-scope sources in place")

add_custom_target(fmt-check
  COMMAND ${CMAKE_COMMAND} -E env ${QUALITY_ENV} bash ${QUALITY_SCRIPT} format --check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL VERBATIM
  COMMENT "quality: check formatting (no writes)")

add_custom_target(lint
  COMMAND ${CMAKE_COMMAND} -E env ${QUALITY_ENV} bash ${QUALITY_SCRIPT} lint
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL VERBATIM
  COMMENT "quality: lint in-scope sources")

# `quality` runs format then lint sequentially (two COMMANDs = ordered).
add_custom_target(quality
  COMMAND ${CMAKE_COMMAND} -E env ${QUALITY_ENV} bash ${QUALITY_SCRIPT} format
  COMMAND ${CMAKE_COMMAND} -E env ${QUALITY_ENV} bash ${QUALITY_SCRIPT} lint
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL VERBATIM
  COMMENT "quality: format then lint")

# Build the clang-tidy plugin before any target that runs clang-tidy.
if (TARGET wasmos_tidy_plugin)
  add_dependencies(lint wasmos_tidy_plugin)
  add_dependencies(quality wasmos_tidy_plugin)
endif ()

# ---------------------------------------------------------------------------
# IDE index targets
# ---------------------------------------------------------------------------
# The wasmos_add_*_app_target helpers already emit a per-app `<name>_ide` target,
# and the kernel/net_stack declare their own. The shared/support areas below are
# NOT built through those helpers, so their sources were missing from
# compile_commands.json and showed up in CLion as "not in a project target".
#
# wasmos_ide_index globs each area (CONFIGURE_DEPENDS, so a new file is picked up
# on the next configure — no per-file CMake edit for IDE coverage) into an
# EXCLUDE_FROM_ALL OBJECT library. These are never linked, so they cannot affect
# the real build or the boot; they exist only for clangd/CLion indexing and
# clang-tidy. See wasmos_add_ide_c_target above for the shared compile options.
# SOURCES names files explicitly instead of globing a tree, for an area whose
# files need a different include set than their neighbours. EXCLUDE drops those
# same files from a sibling glob so each file lands in exactly one index target.
function(wasmos_ide_index name)
  cmake_parse_arguments(II "" "" "DIRS;INCLUDES;IQUOTE;SOURCES;EXCLUDE;DEFINES" ${ARGN})
  set(_srcs ${II_SOURCES})
  foreach(_d ${II_DIRS})
    file(GLOB_RECURSE _g CONFIGURE_DEPENDS
      "${_d}/*.c" "${_d}/*.cc" "${_d}/*.cpp" "${_d}/*.cxx")
    list(APPEND _srcs ${_g})
  endforeach()
  if (II_EXCLUDE)
    list(REMOVE_ITEM _srcs ${II_EXCLUDE})
  endif ()
  if (_srcs)
    wasmos_add_ide_c_target(${name} SOURCES ${_srcs}
                            INCLUDES ${II_INCLUDES} IQUOTE ${II_IQUOTE}
                            DEFINES ${II_DEFINES})
  endif ()
endfunction()

wasmos_ide_index(wasmos_ide_libc
  DIRS ${LIBC_DIR}
  INCLUDES ${LIBC_DIR}/include ${DRIVER_WASM_DIR}/include ${LIBSYS_WASM_DIR}/include)

wasmos_ide_index(wasmos_ide_libsys
  DIRS ${LIBSYS_WASM_DIR} ${LIBSYS_NATIVE_DIR}
  INCLUDES ${LIBSYS_WASM_DIR}/include ${LIBSYS_NATIVE_DIR}/include
           ${LIBC_DIR}/include ${DRIVER_WASM_DIR}/include)

wasmos_ide_index(wasmos_ide_libui
  DIRS ${CMAKE_SOURCE_DIR}/src/libui
  INCLUDES ${CMAKE_SOURCE_DIR}/src/libui/include ${LIBC_DIR}/include
           ${DRIVER_WASM_DIR}/include)

# The unit tests split into two index targets by how their real compile line
# takes the project libc, because the two forms are mutually exclusive and a
# translation unit that reaches both libcs does not parse (on macOS the SDK
# defines strcpy/memcpy as _FORTIFY_SOURCE macros, which mangle our own
# declarations of those names).
#
# WASMOS_UNIT_LIBC_TESTS are the tests whose compile line puts the libc on -I,
# so <string.h>/<stdio.h>/<stdlib.h> are ours and the host's are never reached.
# They are listed by name because a glob cannot express the distinction; a new
# test lands in the globbed target below and, if it needs the libc on -I, fails
# loudly rather than silently linting against the wrong headers.
set(WASMOS_UNIT_LIBC_TESTS
    ${CMAKE_SOURCE_DIR}/tests/unit/stubs_native_libsys.c
    ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wasm_stdlib.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_math.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_stdio.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_startup_argv.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_stdlib.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_string.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libui_key_decode.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_libui_shim_alloc.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_native_ipc_future.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wasm_coroutine.c
    # Same reason as test_wasm_coroutine.c beside it: this suite includes
    # wasmos/libsys.h, which reaches src/libc/include/string.h through a quoted
    # include. With the host libc also serving the angle includes, the SDK's
    # strcpy fortification macro collides with our declaration.
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_mount.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_extent.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_dir.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_populate.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_read.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wfs_path.c
    ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wfs_block_server.c)

# The scheduler host tests drive the REAL kernel sources, so they must see the
# real kernel headers and nothing else. tests/unit/include holds deliberately
# minimal shims for other tests -- thread.h is `struct thread { uint32_t tid; }`
# and sched_event.h redefines the whole sched_event_type_t enum -- so having that
# directory on the include path makes these files fail to parse with dozens of
# redefinition errors. Their real compile lines already omit it; this keeps the
# lint index matching them. WASMOS_HOST_TEST_SMP is required too: without it
# cpu_local() is the GS-relative inline asm, which is not parseable off x86.
#
# The IPC, poll and futex host tests drive the real kernel sources under the
# same rule, so they share this index: real kernel headers, no tests/unit/include
# shim, and WASMOS_HOST_TEST_SMP so cpu_local() is not the GS-relative asm.
set(WASMOS_UNIT_SCHED_TESTS
    ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_runqueue.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_event.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_sched_concurrency.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_ipc_concurrency.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_poll.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_futex.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_syscall_ipc.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_idtable.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_block_buffer.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_value.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_kenv.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_buffer.c)

wasmos_ide_index(wasmos_ide_unit_sched
  SOURCES ${WASMOS_UNIT_SCHED_TESTS}
  IQUOTE ${KERNEL_DIR}/include ${WASM3_DIR}/source
         ${CMAKE_SOURCE_DIR}/abi/generated/c
  DEFINES WASMOS_HOST_TEST_SMP=1 WASMOS_SCHED_CALLER_CPU_BIAS=1
          WASMOS_IPC_TEST_SEAMS=1 WASMOS_FUTEX_TEST_SEAMS=1 WASMOS_IDTABLE_TEST_SEAMS=1
          WASMOS_POLL_TEST_SEAMS=1
          WASMOS_SYSCALL_TEST_SEAMS=1)

# test_hostcall_ipc.cpp drives the real kernel IPC host-call shims, so it takes
# the real kernel headers and not the tests/unit/include shims, exactly like the
# sched tests above. It is a separate target rather than another entry in that
# list for two reasons its real compile line makes explicit: it needs KERNEL_DIR
# itself on -iquote (it reaches the backend shims by subdirectory,
# "warp/link_ipc.h" and "wasm3/link_ipc.h", which KERNEL_DIR/include does not
# serve), and it passes none of the *_TEST_SEAMS defines, which change what the
# kernel headers declare.
wasmos_ide_index(wasmos_ide_unit_hostcall_ipc
  SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_ipc.cpp
  IQUOTE ${KERNEL_DIR} ${KERNEL_DIR}/include ${WASM3_DIR}/source
         ${CMAKE_SOURCE_DIR}/abi/generated/c ${DRIVER_WASM_DIR}/include
  DEFINES WASMOS_HOST_TEST_SMP=1)

# test_hostcall_dma.cpp is the same shape as the IPC target above and needs the
# same -iquote set, but it is separate because it passes NO defines: its real
# compile line does not set WASMOS_HOST_TEST_SMP, and indexing it with a define
# the build never uses would lint it against different kernel headers than the
# ones it is compiled with.
wasmos_ide_index(wasmos_ide_unit_hostcall_dma
  SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_dma.cpp
  IQUOTE ${KERNEL_DIR} ${KERNEL_DIR}/include ${WASM3_DIR}/source
         ${CMAKE_SOURCE_DIR}/abi/generated/c ${DRIVER_WASM_DIR}/include
         ${CMAKE_SOURCE_DIR}/tests/unit)

# test_process_lifecycle.c and its platform stubs link the real process.c, so
# they take the real kernel headers and the WASMOS_PROCESS_TEST_SEAMS define its
# compile line passes -- indexing them without it selects a different process.c
# than the one that is built.
wasmos_ide_index(wasmos_ide_unit_process_lifecycle
  SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_process_lifecycle.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_process_platform.c
  IQUOTE ${KERNEL_DIR} ${KERNEL_DIR}/include ${WASM3_DIR}/source
         ${CMAKE_SOURCE_DIR}/abi/generated/c ${DRIVER_WASM_DIR}/include
         ${CMAKE_SOURCE_DIR}/tests/unit
  DEFINES WASMOS_HOST_TEST_SMP=1 WASMOS_PROCESS_TEST_SEAMS=1 WASMOS_TEST_NCPU=4)

# test_warp_wasm_coroutine.cpp links nothing of WASMOS: it drives the vendored
# WARP JIT directly and resolves its headers relative to WARP_DIR
# ("src/WasmModule/WasmModule.hpp"). The defines are the subset of
# WARP_AOT_FLAGS that WARP's headers branch on; without them the headers select
# a different configuration than the real compile line does. Indexed in every
# configure, not only a WARP one -- libs/warp is always in the tree, and the
# real target additionally builds only on an x86_64 host.
wasmos_ide_index(wasmos_ide_unit_warp
  SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_warp_wasm_coroutine.cpp
  INCLUDES ${WARP_DIR} ${WARP_DIR}/src ${CMAKE_SOURCE_DIR}/abi/generated/c
  DEFINES JIT_TARGET_X86_64 ACTIVE_STACK_OVERFLOW_CHECK=1
          LINEAR_MEMORY_BOUNDS_CHECKS=1 VB_POSIX VB_WIN32_OR_POSIX NVALGRIND)

wasmos_ide_index(wasmos_ide_unit_libc
  SOURCES ${WASMOS_UNIT_LIBC_TESTS}
  INCLUDES ${LIBC_DIR}/include ${LIBC_DIR}/src
           ${CMAKE_SOURCE_DIR}/src/libui/include
           ${DRIVER_WASM_DIR}/include
           ${LIBSYS_WASM_DIR}/include ${LIBSYS_NATIVE_DIR}/include
           ${DRIVER_WASM_DIR}/fs_wfs
           ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
           ${CMAKE_SOURCE_DIR}/abi/generated/c)

# Every other unit test takes the host libc for <angle> includes, matching those
# tests' own `-iquote ${LIBC_DIR}/include`, so all project dirs go on -iquote:
# our stdlib.h/stdio.h/string.h — and src/kernel/include's freestanding ones —
# must not shadow the host headers these tests compile against (the kernel's
# stdlib.h declares no strtol, and none of ours declare stderr/fprintf/abort).
# LIBC_DIR/include still precedes KERNEL_DIR/include so a quoted "stdlib.h"
# finds the fuller libc one. WASM3_DIR is needed because kernel headers reach
# wasm3.h on the interpreter backend. The service dirs carry the headers the
# service-level unit tests include directly.
# test_go_abi_sizes.c describes the wasm32 ABI and only compiles under
# --target=wasm32; indexed with the host flags every assertion in it fires,
# correctly but uselessly. The unit-test target builds it with the right target.
wasmos_ide_index(wasmos_ide_unit
  DIRS ${CMAKE_SOURCE_DIR}/tests/unit
  EXCLUDE ${WASMOS_UNIT_LIBC_TESTS} ${WASMOS_UNIT_SCHED_TESTS}
          ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_ipc.cpp
          ${CMAKE_SOURCE_DIR}/tests/unit/test_hostcall_dma.cpp
          ${CMAKE_SOURCE_DIR}/tests/unit/test_process_lifecycle.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_process_platform.c
          ${CMAKE_SOURCE_DIR}/tests/unit/test_warp_wasm_coroutine.cpp
          ${CMAKE_SOURCE_DIR}/tests/unit/test_go_abi_sizes.c
  IQUOTE ${CMAKE_SOURCE_DIR}/tests/unit/include ${LIBC_DIR}/include
         ${KERNEL_DIR}/include ${DRIVER_WASM_DIR}/include
         ${LIBSYS_WASM_DIR}/include ${LIBSYS_NATIVE_DIR}/include
         ${CMAKE_SOURCE_DIR}/abi/generated/c
         ${SERVICES_DIR}/fs_manager ${SERVICES_DIR}/device_manager
         ${SERVICES_DIR}/net_stack ${SERVICES_DIR}/cli
         ${DRIVER_WASM_DIR}/fs_fat ${DRIVER_WASM_DIR}/fs_wfs
         ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
         ${WASM3_DIR} ${WASM3_DIR}/source)

# The WFS format layer. fs_fat is indexed by the `<name>_ide` target its
# wasmos_add_wasm_c_app_target emits; fs_wfs has no app target yet, so this is
# what keeps its sources out of "not in a project target" and inside the lint
# gate. Retire it once the driver's app target exists.
wasmos_ide_index(wasmos_ide_fs_wfs
  DIRS ${DRIVER_WASM_DIR}/fs_wfs
  INCLUDES ${DRIVER_WASM_DIR}/fs_wfs ${CMAKE_SOURCE_DIR}/abi/generated/c
           ${LIBSYS_WASM_DIR}/include ${LIBC_DIR}/include
           ${DRIVER_WASM_DIR}/include)

wasmos_ide_index(wasmos_ide_tools
  DIRS ${CMAKE_SOURCE_DIR}/src/tools
  INCLUDES ${WARP_DIR} ${WARP_DIR}/src ${DRIVER_WASM_DIR}/include
           ${KERNEL_DIR}/include ${CMAKE_SOURCE_DIR}/abi/generated/c
           ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs ${DRIVER_WASM_DIR}/fs_wfs)
