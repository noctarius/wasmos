cmake_minimum_required(VERSION 3.20)

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

set(WASM_CHARDEV_DRIVER_SRC ${DRIVER_WASM_DIR}/chardev/chardev_server.c)
set(WASM_CHARDEV_DRIVER_WASM ${BUILD_DIR}/chardev_server.wasm)
set(WASM_CHARDEV_DRIVER_BLOB ${BUILD_DIR}/chardev_server_blob.o)
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
set(WASM_ATA_DRIVER_SRC ${DRIVER_WASM_DIR}/ata/ata.c)
set(WASM_ATA_DRIVER_WASM ${BUILD_DIR}/ata.wasm)
set(WASM_ATA_APP ${BUILD_DIR}/ata.wap)
set(WASM_FAT_DRIVER_SRC ${DRIVER_WASM_DIR}/fs_fat/fs_fat.c)
set(WASM_FAT_DRIVER_WASM ${BUILD_DIR}/fs_fat.wasm)
set(WASM_FAT_APP ${BUILD_DIR}/fs_fat.wap)
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
set(WASM_VIRTIO_SERIAL_DRIVER_SRC ${DRIVER_WASM_DIR}/virtio_serial/virtio_serial.c)
set(WASM_VIRTIO_SERIAL_DRIVER_WASM ${BUILD_DIR}/virtio_serial.wasm)
set(WASM_VIRTIO_SERIAL_APP ${BUILD_DIR}/virtio_serial.wap)
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
set(WASMOS_SCRIPT_ENGINE_SRC ${LIBC_DIR}/src/script.c)
set(WASM_INIT_APP_WASM ${BUILD_DIR}/init.wasm)
set(WASM_INIT_APP ${BUILD_DIR}/sysinit.wap)
set(WASM_CLI_APP_SRC ${SERVICES_DIR}/cli/cli.c)
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
set(WASM_HELLO_GO_SRC ${EXAMPLES_GO_DIR}/hello/hello_go.go)
set(WASM_HELLO_GO_WASM ${BUILD_DIR}/hello_go.wasm)
set(WASM_HELLO_GO_APP ${BUILD_DIR}/hello_go.wap)
set(WASM_HELLO_ZIG_SRC ${EXAMPLES_ZIG_DIR}/hello/hello_zig.zig)
set(WASM_HELLO_ZIG_WASM ${BUILD_DIR}/hello_zig.wasm)
set(WASM_HELLO_ZIG_APP ${BUILD_DIR}/hello_zig.wap)
set(WASM_CALCULATOR_SRC ${EXAMPLES_ZIG_DIR}/calculator/calculator.zig)
set(WASM_CALCULATOR_WASM ${BUILD_DIR}/calculator.wasm)
set(WASM_CALCULATOR_APP ${BUILD_DIR}/calculator.wap)
set(AS_HELLO_SRC ${EXAMPLES_AS_DIR}/hello/hello.ts)
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

function(wasmos_add_ide_c_target target_name)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES INCLUDES)
  cmake_parse_arguments(WASMOS_IDE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if (NOT WASMOS_IDE_SOURCES)
    message(FATAL_ERROR "wasmos_add_ide_c_target(${target_name}) requires SOURCES")
  endif ()

  add_library(${target_name} OBJECT EXCLUDE_FROM_ALL ${WASMOS_IDE_SOURCES})
  if (WASMOS_IDE_INCLUDES)
    target_include_directories(${target_name} PRIVATE ${WASMOS_IDE_INCLUDES})
  endif ()
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

if (NOT DEFINED WASMOS_WASM_RUNTIME_WASM3_INITIALIZED)
  set(WASMOS_WASM_RUNTIME_WASM3 ON  CACHE BOOL "Use wasm3 interpreter as WASM runtime" FORCE)
  set(WASMOS_WASM_RUNTIME_WARP  OFF CACHE BOOL "Use WARP JIT compiler as WASM runtime" FORCE)
  set(WASMOS_WASM_RUNTIME_WASM3_INITIALIZED ON CACHE BOOL "Set when WASMOS_WASM_RUNTIME default is initialized" FORCE)
endif ()
option(WASMOS_WASM_RUNTIME_WASM3 "Use wasm3 interpreter as WASM runtime" ON)
option(WASMOS_WASM_RUNTIME_WARP  "Use WARP JIT compiler as WASM runtime" OFF)
option(WASMOS_WARP_RING3 "Execute WARP JIT code in ring-3 user mode (requires WASMOS_WASM_RUNTIME_WARP)" OFF)

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

set(WASMOS_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/math.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

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

function(wasmos_add_wasm_c_app_target target_name)
  set(options NO_BUILTIN STARTUP_SHIM CXX)
  set(oneValueArgs SOURCE OUTPUT_WASM OUTPUT_APP MANIFEST EXPORT STACK_SIZE INITIAL_MEMORY MAX_MEMORY BUILD_COMMENT PACK_COMMENT)
  set(multiValueArgs SOURCES)
  cmake_parse_arguments(WASMOS_APP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(_wasmos_sources ${WASMOS_APP_SOURCES})
  if (NOT _wasmos_sources AND WASMOS_APP_SOURCE)
    set(_wasmos_sources ${WASMOS_APP_SOURCE})
  endif ()

  if (NOT _wasmos_sources OR NOT WASMOS_APP_OUTPUT_WASM OR NOT WASMOS_APP_OUTPUT_APP OR
      NOT WASMOS_APP_MANIFEST OR NOT WASMOS_APP_EXPORT)
    message(FATAL_ERROR "wasmos_add_wasm_c_app_target(${target_name}) requires SOURCE or SOURCES, OUTPUT_WASM, OUTPUT_APP, MANIFEST, EXPORT")
  endif ()

  set(_wasmos_compiler ${CLANG})
  set(_wasmos_extra_flags)
  if (WASMOS_APP_CXX)
    set(_wasmos_compiler ${CMAKE_CXX_COMPILER})
    list(PREPEND _wasmos_extra_flags ${WASMOS_WASM_CXX_FLAGS})
  endif ()
  if (WASMOS_APP_NO_BUILTIN)
    list(APPEND _wasmos_extra_flags -fno-builtin)
  endif ()

  set(_wasmos_libc_sources ${WASMOS_LIBC_C_SOURCES})
  if (WASMOS_APP_STARTUP_SHIM)
    list(APPEND _wasmos_libc_sources ${LIBC_DIR}/src/startup.c)
  endif ()

  set(_stack_size 4096)
  if (WASMOS_APP_STACK_SIZE)
    set(_stack_size ${WASMOS_APP_STACK_SIZE})
  endif ()
  set(_initial_memory 65536)
  if (WASMOS_APP_INITIAL_MEMORY)
    set(_initial_memory ${WASMOS_APP_INITIAL_MEMORY})
  endif ()
  set(_max_memory 65536)
  if (WASMOS_APP_MAX_MEMORY)
    set(_max_memory ${WASMOS_APP_MAX_MEMORY})
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
    COMMAND ${_wasmos_compiler} --target=wasm32 -Oz -nostdlib -ffreestanding
            ${_wasmos_extra_flags}
            ${WASMOS_TRACE_DEFINE}
            -Wl,--no-entry -Wl,--strip-all
            -Wl,-z,stack-size=${_stack_size}
            -Wl,--initial-memory=${_initial_memory}
            -Wl,--max-memory=${_max_memory}
            -Wl,--allow-undefined
            -Wl,--export=${WASMOS_APP_EXPORT}
            -I${CMAKE_SOURCE_DIR}/src/libui/include
            -I${LIBC_DIR}/include
            -I${LIBSYS_WASM_DIR}/include
            -I${DRIVER_WASM_DIR}/include
            ${_wasmos_libc_sources}
            ${_wasmos_sources}
            -o ${WASMOS_APP_OUTPUT_WASM}
    DEPENDS ${_wasmos_sources} ${_wasmos_libc_sources} ${WASMOS_LIBC_INCLUDE_DEPS}
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

set(WASMOS_DOTCONFIG ${CMAKE_SOURCE_DIR}/.config CACHE FILEPATH "Path to Kconfig .config")
set(WASMOS_KCONFIG_IMPORT ${CMAKE_BINARY_DIR}/wasmos_kconfig.cmake)

if (EXISTS ${WASMOS_DOTCONFIG})
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_DOTCONFIG}
            --output ${WASMOS_KCONFIG_IMPORT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE WASMOS_KCONFIG_IMPORT_RESULT
  )
  if (NOT WASMOS_KCONFIG_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to import ${WASMOS_DOTCONFIG} into CMake cache.")
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

# The generic Kconfig WASMOS_RING3 toggle (WARP-only) drives the WARP ring-3
# code gate WASMOS_WARP_RING3.  Derived here, after the Kconfig import and the
# ring3-shadow override, so the selected config is the single source of truth.
if (WASMOS_WASM_RUNTIME_WARP AND WASMOS_RING3)
  set(WASMOS_WARP_RING3 ON  CACHE BOOL "Execute WARP JIT code in ring-3 user mode" FORCE)
else ()
  set(WASMOS_WARP_RING3 OFF CACHE BOOL "Execute WARP JIT code in ring-3 user mode" FORCE)
endif ()

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
set(CFLAGS_EFI ${CFLAGS_COMMON} -target x86_64-unknown-uefi -fshort-wchar -mno-red-zone -B${CLANG_BIN_DIR})
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -target ${KERNEL_TARGET_TRIPLE} -mno-red-zone -mcmodel=kernel -m64 -fno-jump-tables -I${WASM_RUNTIME_INCLUDE_DIR} -I${KERNEL_DIR} -I${KERNEL_DIR}/include -I${DRIVER_WASM_DIR}/include -B${LLD_DIR} -DWASMOS_ENABLE_PREEMPT_GUARD=1 -DWASMOS_ENABLE_SAFEPOINT=1 ${WASM_RUNTIME_DEFINE})
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
  set(LDFLAGS_EFI -fuse-ld=lld -Wl,--subsystem,10 -Wl,--entry,efi_main)
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
    -I${WARP_DIR} -I${WARP_DIR}/src
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
    add_custom_command(
      OUTPUT "${_obj}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${WARP_AOT_OBJ_DIR}"
      COMMAND ${CLANGXX_PATH} ${WARP_AOT_FLAGS} -c "${_src}" -o "${_obj}"
      DEPENDS "${_src}"
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
  add_custom_target(warp_aot DEPENDS ${WARP_AOT_BIN})
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
      # invalidation when the tool changes) AND on the warp_aot TARGET.  The
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
      DEPENDS "${WASM}" "${WARP_AOT_BIN}" warp_aot ${_extra_deps}
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
          ${WASM_CHARDEV_CLIENT_APP}
          ${WASM_INIT_SMOKE_APP}
          ${WASM_NATIVE_CALL_MIN_APP}
          ${WASM_NATIVE_CALL_SMOKE_APP}
          ${WASM_SHMEM_OWNER_APP}
          ${WASM_SHMEM_TARGET_APP}
          ${WASM_ATA_APP}
          ${WASM_FAT_APP}
          ${WASM_FS_INIT_APP}
          ${WASM_FS_MANAGER_APP}
          ${WASM_DEVICE_MANAGER_APP}
          ${WASM_PCI_BUS_APP}
          ${WASM_ACPI_BUS_APP}
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
if (WASMOS_WASM_APP_TARGETS)
  add_dependencies(${WASMOS_WASM_APP_TARGETS} make_wasmos_app)
endif ()
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
add_custom_target(kconfig-defconfig
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
          ${WASMOS_DOTCONFIG}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
          --input ${WASMOS_DOTCONFIG}
          --output ${WASMOS_KCONFIG_IMPORT}
  COMMENT "Seeding ${WASMOS_DOTCONFIG} from configs/wasmos_defconfig"
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
    COMMAND ${CMAKE_COMMAND} -E env KCONFIG_CONFIG=${WASMOS_DOTCONFIG}
            ${WASMOS_MENUCONFIG_BIN} ${CMAKE_SOURCE_DIR}/Kconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_DOTCONFIG}
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
            --config ${WASMOS_DOTCONFIG}
            --defconfig ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_DOTCONFIG}
            --output ${WASMOS_KCONFIG_IMPORT}
    COMMENT "Run menuconfig and import .config into CMake cache"
    USES_TERMINAL
    VERBATIM
  )
endif ()
add_custom_target(kconfiglib-menuconfig
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfiglib_menuconfig.py
          --kconfig ${CMAKE_SOURCE_DIR}/Kconfig
          --config ${WASMOS_DOTCONFIG}
          --defconfig ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
          --input ${WASMOS_DOTCONFIG}
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
  COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/scripts/system/sysinit.rc
      ${BUILD_DIR}/esp/system/sysinit.rc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${USERFS_DIR}
)
if (RUST_ENABLE)
  set(RUST_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_RUST_APP} ${BUILD_DIR}/esp/apps/hello_rust.wap
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
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
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
  ${AS_HELLO_COPY_CMD}
  ${AS_MINESWEEPER_COPY_CMD}
  ${HELLO_C_COPY_CMD}
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
  ${IRQ_ROUTE_DENY_COPY_CMD}
  ${IRQ_ROUTE_ALLOW_COPY_CMD}
  ${RUST_HELLO_COPY_CMD}
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
    COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
            -drive format=raw,file=fat:rw:${USERFS_DIR}
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
  add_custom_target(run-qemu-debug
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
            -S -gdb tcp::${QEMU_GDB_PORT}
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
            -drive format=raw,file=fat:rw:${USERFS_DIR}
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU (paused for GDB on port ${QEMU_GDB_PORT})"
    VERBATIM
  )
  add_custom_target(run-qemu-ui
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_SMP_ARGS} -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
            -drive format=raw,file=fat:rw:${USERFS_DIR}
    DEPENDS bootloader kernel
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
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU halt test"
    VERBATIM
  )
  # Standalone SMP scheduler stress gate. Only meaningful when the kernel was
  # configured with -DWASMOS_SCHED_SMP_STRESS=ON (otherwise the marker is never
  # emitted and this target times out). Runtime-independent: works on any SMP
  # build and does not require a bootable userspace.
  add_custom_target(run-qemu-sched-stress-test
    ${WASMOS_ESP_POPULATE_CMDS}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_sched_stress_test.py
            --ovmf-code ${OVMF_CODE}
            --ovmf-vars ${OVMF_VARS}
            --esp ${BUILD_DIR}/esp
            --smp ${WASMOS_QEMU_SMP_COUNT}
    DEPENDS bootloader kernel
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
            WASMOS_USERFS=${USERFS_DIR}
            WASMOS_QEMU_ISOLATE_ESP=1
            WASMOS_QEMU_SMP_COUNT=${WASMOS_QEMU_SMP_COUNT}
            PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_unittest_suite.py --start-dir tests
    DEPENDS bootloader kernel
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
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU UI test"
    VERBATIM
  )
else ()
  if (OVMF_CODE)
    add_custom_target(run-qemu
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
              -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
              -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
              -drive format=raw,file=fat:rw:${USERFS_DIR}
      DEPENDS bootloader kernel
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running in QEMU"
      VERBATIM
    )
    add_custom_target(run-qemu-debug
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_SMP_ARGS} -nographic -serial mon:stdio
              -S -gdb tcp::${QEMU_GDB_PORT}
              -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
              -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
              -drive format=raw,file=fat:rw:${USERFS_DIR}
      DEPENDS bootloader kernel
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running in QEMU (paused for GDB on port ${QEMU_GDB_PORT})"
      VERBATIM
    )
    add_custom_target(run-qemu-ui
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND qemu-system-x86_64 -m 512M ${WASMOS_QEMU_SMP_ARGS} -serial mon:stdio
              -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
              -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
              -drive format=raw,file=fat:rw:${USERFS_DIR}
      DEPENDS bootloader kernel
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
      DEPENDS bootloader kernel
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
      DEPENDS bootloader kernel
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running SMP scheduler stress test"
      VERBATIM
    )
    add_custom_target(run-qemu-cli-test
      ${WASMOS_ESP_POPULATE_CMDS}
      COMMAND ${CMAKE_COMMAND} -E env
              WASMOS_OVMF_CODE=${OVMF_CODE}
              WASMOS_ESP=${BUILD_DIR}/esp
              WASMOS_USERFS=${USERFS_DIR}
              WASMOS_QEMU_ISOLATE_ESP=1
              WASMOS_QEMU_SMP_COUNT=${WASMOS_QEMU_SMP_COUNT}
              PYTHONDONTWRITEBYTECODE=1
              ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_unittest_suite.py --start-dir tests
      DEPENDS bootloader kernel
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

add_dependencies(make_initfs c_examples drivers services)

set(_wasmos_qemu_targets
  run-qemu run-qemu-ui run-qemu-test run-qemu-cli-test run-qemu-debug
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

add_custom_target(run-kernel-unit-tests
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_list.c
          ${KERNEL_DIR}/list.c
          ${KERNEL_DIR}/list_linked.c
          ${KERNEL_DIR}/list_array_chunk.c
          -o ${BUILD_DIR}/test_list
  COMMAND ${BUILD_DIR}/test_list
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${SERVICES_DIR}/device_manager
          -I${LIBSYS_WASM_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_device_manager_rules.c
          ${SERVICES_DIR}/device_manager/device_manager_rules.c
          -o ${BUILD_DIR}/test_device_manager_rules
  COMMAND ${BUILD_DIR}/test_device_manager_rules
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${SERVICES_DIR}/fs_manager
          ${CMAKE_SOURCE_DIR}/tests/unit/test_fs_manager_path.c
          ${SERVICES_DIR}/fs_manager/fs_manager_path.c
          -o ${BUILD_DIR}/test_fs_manager_path
  COMMAND ${BUILD_DIR}/test_fs_manager_path
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${KERNEL_DIR}/include
          ${CMAKE_SOURCE_DIR}/tests/unit/test_user_mutex.c
          -o ${BUILD_DIR}/test_user_mutex
  COMMAND ${BUILD_DIR}/test_user_mutex
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_stdio.c
          ${LIBC_DIR}/src/stdio.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_libc_stdio
  COMMAND ${BUILD_DIR}/test_libc_stdio
  COMMAND ${CLANG} -std=c11 -Wall -Wextra -Werror -Wno-error=c23-extensions
          -Wno-error=implicit-function-declaration -Wno-sign-compare
          "-D__builtin_wasm_memory_size(x)=((int)(0))"
          "-D__builtin_wasm_memory_grow(x,y)=((int)(-1))"
          -I${LIBC_DIR}/include
          -I${LIBC_DIR}/src
          ${CMAKE_SOURCE_DIR}/tests/unit/test_libc_stdlib.c
          ${CMAKE_SOURCE_DIR}/tests/unit/stubs_wasm_stdlib.c
          ${LIBC_DIR}/src/stdlib.c
          ${LIBC_DIR}/src/string.c
          -o ${BUILD_DIR}/test_libc_stdlib
  COMMAND ${BUILD_DIR}/test_libc_stdlib
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running host kernel unit tests"
  VERBATIM
)
