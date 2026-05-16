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
set(LIBC_DIR ${CMAKE_SOURCE_DIR}/lib/libc)
set(WASM_DIR ${CMAKE_SOURCE_DIR}/libs/wasm)
set(DRIVER_WASM_DIR ${CMAKE_SOURCE_DIR}/src/drivers)
set(EXAMPLES_C_DIR ${CMAKE_SOURCE_DIR}/examples/c)
set(EXAMPLES_GO_DIR ${CMAKE_SOURCE_DIR}/examples/go)
set(EXAMPLES_RUST_DIR ${CMAKE_SOURCE_DIR}/examples/rust)
set(EXAMPLES_AS_DIR ${CMAKE_SOURCE_DIR}/examples/assemblyscript)
set(EXAMPLES_ZIG_DIR ${CMAKE_SOURCE_DIR}/examples/zig)
set(SERVICES_DIR ${CMAKE_SOURCE_DIR}/src/services)

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
set(WASM_FRAMEBUFFER_APP ${BUILD_DIR}/framebuffer.wap)
set(NATIVE_FRAMEBUFFER_DRIVER_SRC ${DRIVER_WASM_DIR}/framebuffer/framebuffer_native.c)
set(NATIVE_FRAMEBUFFER_RENDER_SRC ${DRIVER_WASM_DIR}/framebuffer/render.c)
set(NATIVE_FRAMEBUFFER_DRIVER_ELF ${BUILD_DIR}/framebuffer_native.elf)
set(NATIVE_FRAMEBUFFER_DRIVER_OBJ ${BUILD_DIR}/framebuffer_native.o)
set(NATIVE_FRAMEBUFFER_RENDER_OBJ ${BUILD_DIR}/framebuffer_render.o)
set(WASM_INIT_APP_SRC ${SERVICES_DIR}/sysinit/init.c)
set(WASM_INIT_APP_WASM ${BUILD_DIR}/init.wasm)
set(WASM_INIT_APP ${BUILD_DIR}/sysinit.wap)
set(WASM_CLI_APP_SRC ${SERVICES_DIR}/cli/cli.c)
set(WASM_CLI_APP_WASM ${BUILD_DIR}/cli.wasm)
set(WASM_CLI_APP ${BUILD_DIR}/cli.wap)
set(WASM_DEVICE_MANAGER_SRC ${SERVICES_DIR}/device_manager/device_manager.c)
set(WASM_DEVICE_MANAGER_WASM ${BUILD_DIR}/device_manager.wasm)
set(WASM_DEVICE_MANAGER_APP ${BUILD_DIR}/device_manager.wap)
set(WASM_PCI_BUS_SRC ${SERVICES_DIR}/pci_bus/pci_bus.c)
set(WASM_PCI_BUS_WASM ${BUILD_DIR}/pci_bus.wasm)
set(WASM_PCI_BUS_APP ${BUILD_DIR}/pci_bus.wap)
set(WASM_FS_MANAGER_SRC ${SERVICES_DIR}/fs_manager/fs_manager.c)
set(WASM_FS_MANAGER_WASM ${BUILD_DIR}/fs_manager.wasm)
set(WASM_FS_MANAGER_APP ${BUILD_DIR}/fs_manager.wap)
set(WASM_VT_APP_SRC ${SERVICES_DIR}/vt/vt_main.c)
set(WASM_VT_APP_WASM ${BUILD_DIR}/vt.wasm)
set(WASM_VT_APP ${BUILD_DIR}/vt.wap)
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
set(AS_HELLO_SRC ${EXAMPLES_AS_DIR}/hello/hello.ts)
set(AS_HELLO_WASM ${BUILD_DIR}/hello_as.wasm)
set(AS_HELLO_APP ${BUILD_DIR}/hello_as.wap)

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
  target_compile_definitions(${target_name} PRIVATE WASMOS_TRACE=${WASMOS_TRACE_VALUE})
endfunction()
option(WASMOS_RING3_SMOKE "Enable ring3 smoke probe spawn by default" OFF)
if (WASMOS_RING3_SMOKE)
  set(WASMOS_RING3_SMOKE_VALUE 1)
else ()
  set(WASMOS_RING3_SMOKE_VALUE 0)
endif ()
set(WASMOS_RING3_SMOKE_DEFINE -DWASMOS_RING3_SMOKE_DEFAULT=${WASMOS_RING3_SMOKE_VALUE})
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

project(wasmos C ASM)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include(CMakeParseArguments)

set(WASMOS_DOTCONFIG ${CMAKE_BINARY_DIR}/.config CACHE FILEPATH "Path to Kconfig .config")
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

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

set(CFLAGS_COMMON -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -g)
set(CFLAGS_EFI ${CFLAGS_COMMON} -target x86_64-unknown-uefi -fshort-wchar -mno-red-zone -B${CLANG_BIN_DIR})
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -target ${KERNEL_TARGET_TRIPLE} -mno-red-zone -mcmodel=kernel -m64 -fno-jump-tables -I${WASM_DIR} -I${KERNEL_DIR}/include -I${DRIVER_WASM_DIR}/include -B${LLD_DIR} -DWASMOS_ENABLE_PREEMPT_GUARD=1 -DWASMOS_ENABLE_SAFEPOINT=1)
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
set(WASMOS_APP_PACKER $<TARGET_FILE:make_wasmos_app>)
add_custom_command(
  OUTPUT ${INITFS_IMG} ${INITFS_CONFIG_BIN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/make_initfs.py
          --manifest ${INITFS_TOML}
          --build-dir ${BUILD_DIR}
          --output ${INITFS_IMG}
          --config-output ${INITFS_CONFIG_BIN}
  DEPENDS ${CMAKE_SOURCE_DIR}/scripts/make_initfs.py
          ${INITFS_TOML}
          ${WASM_CHARDEV_CLIENT_APP}
          ${WASM_INIT_SMOKE_APP}
          ${WASM_NATIVE_CALL_MIN_APP}
          ${WASM_NATIVE_CALL_SMOKE_APP}
          ${WASM_SHMEM_OWNER_APP}
          ${WASM_SHMEM_TARGET_APP}
          ${WASM_ATA_APP}
          ${WASM_FAT_APP}
          ${WASM_DEVICE_MANAGER_APP}
          ${WASM_PCI_BUS_APP}
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
find_program(WASMOS_MENUCONFIG_BIN NAMES menuconfig nconfig kconfig-mconf mconf)
if (WASMOS_MENUCONFIG_BIN)
  add_custom_target(menuconfig
    COMMAND ${CMAKE_COMMAND} -E env KCONFIG_CONFIG=${WASMOS_DOTCONFIG}
            ${WASMOS_MENUCONFIG_BIN} ${CMAKE_SOURCE_DIR}/Kconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfig_to_cmake.py
            --input ${WASMOS_DOTCONFIG}
            --output ${WASMOS_KCONFIG_IMPORT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run menuconfig and import .config into CMake cache"
    VERBATIM
  )
else ()
  add_custom_target(menuconfig
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfiglib_menuconfig.py
            --kconfig ${CMAKE_SOURCE_DIR}/Kconfig
            --config ${WASMOS_DOTCONFIG}
            --defconfig ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
    COMMENT "Run menuconfig and import .config into CMake cache"
    VERBATIM
  )
endif ()
add_custom_target(kconfiglib-menuconfig
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/kconfiglib_menuconfig.py
          --kconfig ${CMAKE_SOURCE_DIR}/Kconfig
          --config ${WASMOS_DOTCONFIG}
          --defconfig ${CMAKE_SOURCE_DIR}/configs/wasmos_defconfig
  COMMENT "Run kconfiglib interactive config editor"
  VERBATIM
)
if (AS_ENABLE)
  set(AS_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${AS_HELLO_APP} ${BUILD_DIR}/esp/apps/hello_as.wap
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
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SERIAL_APP} ${BUILD_DIR}/esp/system/drivers/serial.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_KEYBOARD_APP} ${BUILD_DIR}/esp/system/drivers/keyboard.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FRAMEBUFFER_APP} ${BUILD_DIR}/esp/system/drivers/framebuffer.wap
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
endif ()

if (OVMF_VARS AND OVMF_CODE)
  if (OVMF_VARS MATCHES "i386" AND OVMF_CODE MATCHES "x86_64")
    message(WARNING "OVMF_VARS appears to be i386 while OVMF_CODE is x86_64; ignoring OVMF_VARS to avoid boot hangs.")
    set(OVMF_VARS "" CACHE FILEPATH "Path to OVMF_VARS.fd (optional)" FORCE)
  endif ()
endif ()
if (OVMF_VARS)
  add_custom_target(run-qemu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_SERIAL_APP} ${BUILD_DIR}/esp/system/drivers/serial.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_KEYBOARD_APP} ${BUILD_DIR}/esp/system/drivers/keyboard.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -nographic -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
  add_custom_target(run-qemu-debug
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_KEYBOARD_APP} ${BUILD_DIR}/esp/system/drivers/keyboard.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -nographic -serial mon:stdio
            -S -gdb tcp::${QEMU_GDB_PORT}
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive if=pflash,format=raw,file=${OVMF_VARS}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU (paused for GDB on port ${QEMU_GDB_PORT})"
    VERBATIM
  )
  add_custom_target(run-qemu-test
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
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_halt_test.py
            --ovmf-code ${OVMF_CODE}
            --ovmf-vars ${OVMF_VARS}
            --esp ${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU halt test"
    VERBATIM
  )
  if (AS_ENABLE)
  endif ()
add_custom_target(run-qemu-cli-test
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
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${CMAKE_COMMAND} -E env
            WASMOS_OVMF_CODE=${OVMF_CODE}
            WASMOS_OVMF_VARS=${OVMF_VARS}
            WASMOS_ESP=${BUILD_DIR}/esp
            WASMOS_QEMU_ISOLATE_ESP=1
            PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_unittest_suite.py --start-dir tests
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT \"Running QEMU CLI integration tests\"
    VERBATIM
  )
  if (AS_ENABLE)
  endif ()
else ()
  add_custom_target(run-qemu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -nographic -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
  add_custom_target(run-qemu-ui
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
  add_custom_target(run-qemu-debug
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -nographic -serial mon:stdio
            -S -gdb tcp::${QEMU_GDB_PORT}
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU (paused for GDB on port ${QEMU_GDB_PORT})"
    VERBATIM
  )
  if (AS_ENABLE)
  endif ()
  add_custom_target(run-qemu-test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_halt_test.py
            --ovmf-code ${OVMF_CODE}
            --esp ${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running QEMU halt test"
    VERBATIM
  )
  if (AS_ENABLE)
  endif ()
  add_custom_target(run-qemu-cli-test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    ${INITFS_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${CHARDEV_PREEMPT_COPY_CMD}
    ${INIT_SMOKE_COPY_CMD}
    ${FS_OPEN_SMOKE_COPY_CMD}
    ${FS_WRITE_SMOKE_COPY_CMD}
    ${NATIVE_CALL_MIN_COPY_CMD}
    ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    ${LARGE_READ_COPY_CMD}
    ${WRITE_SMOKE_FILE_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${CMAKE_COMMAND} -E env
            WASMOS_OVMF_CODE=${OVMF_CODE}
            WASMOS_OVMF_VARS=${OVMF_VARS}
            WASMOS_ESP=${BUILD_DIR}/esp
            WASMOS_QEMU_ISOLATE_ESP=1
            PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_unittest_suite.py --start-dir tests
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT \"Running QEMU CLI integration tests\"
    VERBATIM
  )
set(QEMU_UI_OVMF_VARS_ARG "")
if (OVMF_VARS)
  set(QEMU_UI_OVMF_VARS_ARG "--ovmf-vars ${OVMF_VARS}")
endif ()

add_custom_target(run-qemu-ui-test
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
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
  ${AS_HELLO_COPY_CMD}
  ${HELLO_C_COPY_CMD}
  ${CHARDEV_PREEMPT_COPY_CMD}
  ${INIT_SMOKE_COPY_CMD}
  ${FS_OPEN_SMOKE_COPY_CMD}
  ${FS_WRITE_SMOKE_COPY_CMD}
  ${NATIVE_CALL_MIN_COPY_CMD}
  ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
  ${RUST_HELLO_COPY_CMD}
  ${GO_HELLO_COPY_CMD}
  ${ZIG_HELLO_COPY_CMD}
  ${LARGE_READ_COPY_CMD}
  ${WRITE_SMOKE_FILE_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/qemu_ui_test.py
            --ovmf-code ${OVMF_CODE}
            ${QEMU_UI_OVMF_VARS_ARG}
            --esp ${BUILD_DIR}/esp
  DEPENDS bootloader kernel
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running QEMU UI test"
  VERBATIM
)
endif ()
add_custom_target(run-qemu-ring3-check
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
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
  ${AS_HELLO_COPY_CMD}
  ${HELLO_C_COPY_CMD}
  ${CHARDEV_PREEMPT_COPY_CMD}
  ${INIT_SMOKE_COPY_CMD}
  ${FS_OPEN_SMOKE_COPY_CMD}
  ${FS_WRITE_SMOKE_COPY_CMD}
  ${NATIVE_CALL_MIN_COPY_CMD}
  ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
  ${RUST_HELLO_COPY_CMD}
  ${GO_HELLO_COPY_CMD}
  ${ZIG_HELLO_COPY_CMD}
  ${LARGE_READ_COPY_CMD}
  ${WRITE_SMOKE_FILE_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
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
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
  ${AS_HELLO_COPY_CMD}
  ${HELLO_C_COPY_CMD}
  ${CHARDEV_PREEMPT_COPY_CMD}
  ${INIT_SMOKE_COPY_CMD}
  ${FS_OPEN_SMOKE_COPY_CMD}
  ${FS_WRITE_SMOKE_COPY_CMD}
  ${NATIVE_CALL_MIN_COPY_CMD}
  ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
  ${RUST_HELLO_COPY_CMD}
  ${GO_HELLO_COPY_CMD}
  ${ZIG_HELLO_COPY_CMD}
  ${LARGE_READ_COPY_CMD}
  ${WRITE_SMOKE_FILE_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
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
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/apps/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_VT_APP} ${BUILD_DIR}/esp/system/services/vt.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_DEVICE_MANAGER_APP} ${BUILD_DIR}/esp/system/services/device_manager.wap
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_PCI_BUS_APP} ${BUILD_DIR}/esp/system/services/pci_bus.wap
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wap
  ${AS_HELLO_COPY_CMD}
  ${HELLO_C_COPY_CMD}
  ${CHARDEV_PREEMPT_COPY_CMD}
  ${INIT_SMOKE_COPY_CMD}
  ${FS_OPEN_SMOKE_COPY_CMD}
  ${FS_WRITE_SMOKE_COPY_CMD}
  ${NATIVE_CALL_MIN_COPY_CMD}
  ${NATIVE_CALL_SMOKE_COPY_CMD}
  ${SHMEM_OWNER_COPY_CMD}
  ${SHMEM_TARGET_COPY_CMD}
    ${IRQ_ROUTE_DENY_COPY_CMD}
    ${IRQ_ROUTE_ALLOW_COPY_CMD}
  ${RUST_HELLO_COPY_CMD}
  ${GO_HELLO_COPY_CMD}
  ${ZIG_HELLO_COPY_CMD}
  ${LARGE_READ_COPY_CMD}
  ${WRITE_SMOKE_FILE_COPY_CMD}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
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

add_custom_target(run-qemu-ring3-test
  COMMAND ${CMAKE_COMMAND}
          -S ${CMAKE_SOURCE_DIR}
          -B ${BUILD_DIR}/ring3
          -DCLANG=${CLANG}
          -DLLD=${LLD}
          -DOBJCOPY=${OBJCOPY}
          -DOVMF_CODE=${OVMF_CODE}
          -DOVMF_VARS=${OVMF_VARS}
          -DWASMOS_TRACE=${WASMOS_TRACE}
          -DWASMOS_RING3_SMOKE=ON
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR}/ring3 --target run-qemu-ring3-check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Configure/build shadow ring3 tree and run ring3 smoke marker test"
  VERBATIM
)
add_custom_target(run-qemu-ring3-threading-test
  COMMAND ${CMAKE_COMMAND}
          -S ${CMAKE_SOURCE_DIR}
          -B ${BUILD_DIR}/ring3-threading
          -DCLANG=${CLANG}
          -DLLD=${LLD}
          -DOBJCOPY=${OBJCOPY}
          -DOVMF_CODE=${OVMF_CODE}
          -DOVMF_VARS=${OVMF_VARS}
          -DWASMOS_TRACE=${WASMOS_TRACE}
          -DWASMOS_RING3_SMOKE=ON
          -DWASMOS_RING3_THREAD_LIFECYCLE_SMOKE=ON
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR}/ring3-threading --target run-qemu-ring3-threading-check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Configure/build shadow threading ring3 tree and run lifecycle marker test"
  VERBATIM
)
add_custom_target(run-qemu-ring3-fault-storm-test
  COMMAND ${CMAKE_COMMAND}
          -S ${CMAKE_SOURCE_DIR}
          -B ${BUILD_DIR}/ring3-fault-storm
          -DCLANG=${CLANG}
          -DLLD=${LLD}
          -DOBJCOPY=${OBJCOPY}
          -DOVMF_CODE=${OVMF_CODE}
          -DOVMF_VARS=${OVMF_VARS}
          -DWASMOS_TRACE=${WASMOS_TRACE}
          -DWASMOS_RING3_SMOKE=ON
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR}/ring3-fault-storm --target run-qemu-ring3-fault-storm-check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Configure/build shadow ring3 fault-storm tree and run dedicated marker test"
  VERBATIM
)
add_custom_target(strict-ring3
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target run-qemu-test
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target run-qemu-ring3-test
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Run strict ring3 gate profile (run-qemu-test + run-qemu-ring3-test)"
  VERBATIM
)
add_dependencies(run-qemu c_examples services drivers)
add_dependencies(run-qemu-test c_examples services drivers)
add_dependencies(run-qemu-cli-test c_examples services drivers)
add_dependencies(run-qemu-debug c_examples services drivers)
add_dependencies(run-qemu-ring3-check c_examples services drivers)
add_dependencies(run-qemu-ring3-threading-check c_examples services drivers)
add_dependencies(run-qemu-ring3-fault-storm-check c_examples services drivers)
add_dependencies(make_initfs c_examples)
add_dependencies(run-qemu make_initfs)
add_dependencies(run-qemu-ui make_initfs)
add_dependencies(run-qemu-test make_initfs)
add_dependencies(run-qemu-cli-test make_initfs)
add_dependencies(run-qemu-debug make_initfs)
add_dependencies(run-qemu-ring3-check make_initfs)
add_dependencies(run-qemu-ring3-threading-check make_initfs)
add_dependencies(run-qemu-ring3-fault-storm-check make_initfs)

if (TARGET assemblyscript_examples)
  add_dependencies(run-qemu assemblyscript_examples)
  add_dependencies(run-qemu-test assemblyscript_examples)
  add_dependencies(run-qemu-cli-test assemblyscript_examples)
  add_dependencies(run-qemu-debug assemblyscript_examples)
  add_dependencies(run-qemu-ring3-check assemblyscript_examples)
  add_dependencies(run-qemu-ring3-threading-check assemblyscript_examples)
  add_dependencies(run-qemu-ring3-fault-storm-check assemblyscript_examples)
endif ()
if (TARGET rust_examples)
  add_dependencies(run-qemu rust_examples)
  add_dependencies(run-qemu-test rust_examples)
  add_dependencies(run-qemu-cli-test rust_examples)
  add_dependencies(run-qemu-debug rust_examples)
  add_dependencies(run-qemu-ring3-check rust_examples)
  add_dependencies(run-qemu-ring3-threading-check rust_examples)
  add_dependencies(run-qemu-ring3-fault-storm-check rust_examples)
endif ()
if (TARGET go_examples)
  add_dependencies(run-qemu go_examples)
  add_dependencies(run-qemu-test go_examples)
  add_dependencies(run-qemu-cli-test go_examples)
  add_dependencies(run-qemu-debug go_examples)
  add_dependencies(run-qemu-ring3-check go_examples)
  add_dependencies(run-qemu-ring3-threading-check go_examples)
  add_dependencies(run-qemu-ring3-fault-storm-check go_examples)
endif ()
if (TARGET zig_examples)
  add_dependencies(run-qemu zig_examples)
  add_dependencies(run-qemu-test zig_examples)
  add_dependencies(run-qemu-cli-test zig_examples)
  add_dependencies(run-qemu-debug zig_examples)
  add_dependencies(run-qemu-ring3-check zig_examples)
  add_dependencies(run-qemu-ring3-threading-check zig_examples)
  add_dependencies(run-qemu-ring3-fault-storm-check zig_examples)
endif ()

if (TARGET run-qemu-ui-test)
  add_dependencies(run-qemu-ui-test c_examples services drivers)
  add_dependencies(run-qemu-ui-test make_initfs)
  if (TARGET assemblyscript_examples)
    add_dependencies(run-qemu-ui-test assemblyscript_examples)
  endif ()
  if (TARGET rust_examples)
    add_dependencies(run-qemu-ui-test rust_examples)
  endif ()
  if (TARGET go_examples)
    add_dependencies(run-qemu-ui-test go_examples)
  endif ()
  if (TARGET zig_examples)
    add_dependencies(run-qemu-ui-test zig_examples)
  endif ()
endif ()
