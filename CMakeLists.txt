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
set(WASM_DIR ${CMAKE_SOURCE_DIR}/libs/wasm)
set(WAMR_DIR ${WASM_DIR}/wasm-micro-runtime)
set(DRIVER_WASM_DIR ${CMAKE_SOURCE_DIR}/src/drivers)
set(EXAMPLES_C_DIR ${CMAKE_SOURCE_DIR}/examples/c)
set(EXAMPLES_GO_DIR ${CMAKE_SOURCE_DIR}/examples/go)
set(EXAMPLES_RUST_DIR ${CMAKE_SOURCE_DIR}/examples/rust)
set(EXAMPLES_AS_DIR ${CMAKE_SOURCE_DIR}/examples/assemblyscript)
set(EXAMPLES_ZIG_DIR ${CMAKE_SOURCE_DIR}/examples/zig)
set(SERVICES_DIR ${CMAKE_SOURCE_DIR}/src/services)

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

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -target ${KERNEL_TARGET_TRIPLE} -mno-red-zone -mcmodel=kernel -m64 -I${WASM_DIR} -I${KERNEL_DIR}/include -I${DRIVER_WASM_DIR}/include -B${LLD_DIR})
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

if (OVMF_VARS AND NOT EXISTS ${OVMF_VARS})
  message(WARNING "OVMF_VARS '${OVMF_VARS}' not found; continuing without vars.")
  set(OVMF_VARS "" CACHE FILEPATH "Path to OVMF_VARS.fd (optional)" FORCE)
endif ()
set(LDFLAGS_KERNEL -nostdlib -T ${KERNEL_DIR}/arch/x86_64/linker.ld)

set(KERNEL_OBJS
  ${BUILD_DIR}/entry.o
  ${BUILD_DIR}/cpu_isr.o
  ${BUILD_DIR}/kernel.o
  ${BUILD_DIR}/cpu.o
  ${BUILD_DIR}/memory.o
  ${BUILD_DIR}/paging.o
  ${BUILD_DIR}/physmem.o
  ${BUILD_DIR}/ipc.o
  ${BUILD_DIR}/process.o
  ${BUILD_DIR}/spinlock.o
  ${BUILD_DIR}/memory_service.o
  ${BUILD_DIR}/irq.o
  ${BUILD_DIR}/process_manager.o
  ${BUILD_DIR}/wasmos_app.o
  ${BUILD_DIR}/wasm_driver.o
  ${BUILD_DIR}/wamr_context.o
  ${BUILD_DIR}/wasm_chardev.o
  ${BUILD_DIR}/serial.o
  ${BUILD_DIR}/wamr_runtime.o
)

set(KERNEL_LINK_LIBS "")

set(BOOT_SOURCES
  ${BOOT_DIR}/boot.c
)

set(KERNEL_SOURCES
  ${KERNEL_DIR}/arch/x86_64/entry.S
  ${KERNEL_DIR}/arch/x86_64/cpu_isr.S
  ${KERNEL_DIR}/kernel.c
  ${KERNEL_DIR}/cpu.c
  ${KERNEL_DIR}/memory.c
  ${KERNEL_DIR}/paging.c
  ${KERNEL_DIR}/physmem.c
  ${KERNEL_DIR}/ipc.c
  ${KERNEL_DIR}/process.c
  ${KERNEL_DIR}/spinlock.c
  ${KERNEL_DIR}/memory_service.c
  ${KERNEL_DIR}/irq.c
  ${KERNEL_DIR}/process_manager.c
  ${KERNEL_DIR}/wasmos_app.c
  ${KERNEL_DIR}/wasm_driver.c
  ${KERNEL_DIR}/wamr_context.c
  ${KERNEL_DIR}/wasm_chardev.c
  ${KERNEL_DIR}/serial.c
  ${WASM_DIR}/wamr_runtime.c
)

# Editor/indexer targets for CLion so sources are part of a CMake target.
add_library(bootloader_sources INTERFACE)
target_sources(bootloader_sources INTERFACE ${BOOT_SOURCES} ${BOOT_DIR}/uefi.h ${BOOT_DIR}/elf.h ${KERNEL_DIR}/include/boot.h)
target_include_directories(bootloader_sources INTERFACE ${BOOT_DIR} ${KERNEL_DIR}/include)

add_library(kernel_sources INTERFACE)
target_sources(kernel_sources INTERFACE
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/memory.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wamr_context.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_init.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_stub.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_internal.h
  ${WASM_DIR}/wamr_runtime.h
)
target_include_directories(kernel_sources INTERFACE ${KERNEL_DIR}/include ${WASM_DIR} ${DRIVER_WASM_DIR}/include)

# IDE aggregation targets for CLion indexing (not part of the build).
add_library(bootloader_ide OBJECT EXCLUDE_FROM_ALL
  ${BOOT_SOURCES}
  ${BOOT_DIR}/uefi.h
  ${BOOT_DIR}/elf.h
  ${KERNEL_DIR}/include/boot.h
)
target_include_directories(bootloader_ide PRIVATE
  ${BOOT_DIR}
  ${KERNEL_DIR}/include
)

add_library(kernel_ide OBJECT EXCLUDE_FROM_ALL
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/memory.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wamr_context.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  ${DRIVER_WASM_DIR}/chardev/chardev_server.c
  ${EXAMPLES_C_DIR}/chardev_client/chardev_client.c
  ${EXAMPLES_C_DIR}/hello/hello_c.c
  ${DRIVER_WASM_DIR}/ata/ata.c
  ${DRIVER_WASM_DIR}/fs_fat/fs_fat.c
  ${SERVICES_DIR}/sysinit/init.c
  ${SERVICES_DIR}/cli/cli.c
  ${SERVICES_DIR}/hw_discovery/hw_discovery.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_init.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_stub.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_internal.h
  ${WASM_DIR}/wamr_runtime.h
)
target_include_directories(kernel_ide PRIVATE
  ${KERNEL_DIR}/include
  ${DRIVER_WASM_DIR}/include
  ${WAMR_DIR}/core/iwasm/include
  ${WASM_DIR}
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos
)

set(WASM_CHARDEV_DRIVER_SRC ${DRIVER_WASM_DIR}/chardev/chardev_server.c)
set(WASM_CHARDEV_DRIVER_WASM ${BUILD_DIR}/chardev_server.wasm)
set(WASM_CHARDEV_DRIVER_BLOB ${BUILD_DIR}/chardev_server_blob.o)
set(WASM_CHARDEV_CLIENT_SRC ${EXAMPLES_C_DIR}/chardev_client/chardev_client.c)
set(WASM_CHARDEV_CLIENT_WASM ${BUILD_DIR}/chardev_client.wasm)
set(WASM_CHARDEV_CLIENT_BLOB ${BUILD_DIR}/chardev_client_blob.o)
set(WASM_CHARDEV_CLIENT_APP ${BUILD_DIR}/chardev_client.wasmosapp)
set(WASM_ATA_DRIVER_SRC ${DRIVER_WASM_DIR}/ata/ata.c)
set(WASM_ATA_DRIVER_WASM ${BUILD_DIR}/ata.wasm)
set(WASM_ATA_APP ${BUILD_DIR}/ata.wasmosapp)
set(WASM_FAT_DRIVER_SRC ${DRIVER_WASM_DIR}/fs_fat/fs_fat.c)
set(WASM_FAT_DRIVER_WASM ${BUILD_DIR}/fs_fat.wasm)
set(WASM_FAT_APP ${BUILD_DIR}/fs_fat.wasmosapp)
set(WASM_INIT_APP_SRC ${SERVICES_DIR}/sysinit/init.c)
set(WASM_INIT_APP_WASM ${BUILD_DIR}/init.wasm)
set(WASM_INIT_APP ${BUILD_DIR}/sysinit.wasmosapp)
set(WASM_CLI_APP_SRC ${SERVICES_DIR}/cli/cli.c)
set(WASM_CLI_APP_WASM ${BUILD_DIR}/cli.wasm)
set(WASM_CLI_APP ${BUILD_DIR}/cli.wasmosapp)
set(WASM_HW_DISCOVERY_SRC ${SERVICES_DIR}/hw_discovery/hw_discovery.c)
set(WASM_HW_DISCOVERY_WASM ${BUILD_DIR}/hw_discovery.wasm)
set(WASM_HW_DISCOVERY_APP ${BUILD_DIR}/hw_discovery.wasmosapp)
set(WASM_HELLO_C_SRC ${EXAMPLES_C_DIR}/hello/hello_c.c)
set(WASM_HELLO_C_WASM ${BUILD_DIR}/hello_c.wasm)
set(WASM_HELLO_C_APP ${BUILD_DIR}/hello_c.wasmosapp)
set(WASM_HELLO_RUST_SRC ${EXAMPLES_RUST_DIR}/hello/hello_rust.rs)
set(WASM_HELLO_RUST_WASM ${BUILD_DIR}/hello_rust.wasm)
set(WASM_HELLO_RUST_APP ${BUILD_DIR}/hello_rust.wasmosapp)
set(WASM_HELLO_GO_SRC ${EXAMPLES_GO_DIR}/hello/hello_go.go)
set(WASM_HELLO_GO_WASM ${BUILD_DIR}/hello_go.wasm)
set(WASM_HELLO_GO_APP ${BUILD_DIR}/hello_go.wasmosapp)
set(WASM_HELLO_ZIG_SRC ${EXAMPLES_ZIG_DIR}/hello/hello_zig.zig)
set(WASM_HELLO_ZIG_WASM ${BUILD_DIR}/hello_zig.wasm)
set(WASM_HELLO_ZIG_APP ${BUILD_DIR}/hello_zig.wasmosapp)
set(WASMOS_APP_PACKER ${BUILD_DIR}/make_wasmos_app)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--export=chardev_init
          -Wl,--export=chardev_read_byte
          -Wl,--export=chardev_write_byte
          -Wl,--export=chardev_ipc_dispatch
          -I${DRIVER_WASM_DIR}/include
          ${WASM_CHARDEV_DRIVER_SRC}
          -o ${WASM_CHARDEV_DRIVER_WASM}
  DEPENDS ${WASM_CHARDEV_DRIVER_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm chardev driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_DRIVER_BLOB}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          chardev_server.wasm chardev_server_blob.o
  DEPENDS ${WASM_CHARDEV_DRIVER_WASM}
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Embedding wasm chardev driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FAT_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=fat_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_FAT_DRIVER_SRC}
          -o ${WASM_FAT_DRIVER_WASM}
  DEPENDS ${WASM_FAT_DRIVER_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm FAT driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FAT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_FAT_DRIVER_WASM}
          ${WASM_FAT_APP}
          fs-fat
          fat_step
          16
          16
          2
          block
          1
          ipc.basic
          0
  DEPENDS ${WASM_FAT_DRIVER_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm FAT as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_ATA_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=ata_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_ATA_DRIVER_SRC}
          -o ${WASM_ATA_DRIVER_WASM}
  DEPENDS ${WASM_ATA_DRIVER_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm ATA driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_ATA_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_ATA_DRIVER_WASM}
          ${WASM_ATA_APP}
          ata
          ata_step
          16
          16
          1
          -
          0
          ipc.basic
          0
  DEPENDS ${WASM_ATA_DRIVER_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm ATA as WASMOS-APP"
  VERBATIM
)
add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=chardev_client_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_CHARDEV_CLIENT_SRC}
          -o ${WASM_CHARDEV_CLIENT_WASM}
  DEPENDS ${WASM_CHARDEV_CLIENT_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm chardev client module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_INIT_APP_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=init_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_INIT_APP_SRC}
          -o ${WASM_INIT_APP_WASM}
  DEPENDS ${WASM_INIT_APP_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm init module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CLI_APP_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=cli_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_CLI_APP_SRC}
          -o ${WASM_CLI_APP_WASM}
  DEPENDS ${WASM_CLI_APP_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm CLI module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_HW_DISCOVERY_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=hw_discovery_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_HW_DISCOVERY_SRC}
          -o ${WASM_HW_DISCOVERY_WASM}
  DEPENDS ${WASM_HW_DISCOVERY_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm hw-discovery module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_HELLO_C_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=hello_c_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_HELLO_C_SRC}
          -o ${WASM_HELLO_C_WASM}
  DEPENDS ${WASM_HELLO_C_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm hello-c module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_BLOB}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          chardev_client.wasm chardev_client_blob.o
  DEPENDS ${WASM_CHARDEV_CLIENT_WASM}
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Embedding wasm chardev client module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASMOS_APP_PACKER}
  COMMAND cc -O2 ${CMAKE_SOURCE_DIR}/scripts/make_wasmos_app.c -o ${WASMOS_APP_PACKER}
  DEPENDS ${CMAKE_SOURCE_DIR}/scripts/make_wasmos_app.c
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building WASMOS-APP packer tool"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_INIT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_INIT_APP_WASM}
          ${WASM_INIT_APP}
          sysinit
          init_step
          16
          16
          2
          proc
          1
          ipc.basic
          0
  DEPENDS ${WASM_INIT_APP_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm init as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CLI_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_CLI_APP_WASM}
          ${WASM_CLI_APP}
          cli
          cli_step
          16
          16
          4
          proc
          1
          ipc.basic
          0
  DEPENDS ${WASM_CLI_APP_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm CLI as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_HW_DISCOVERY_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_HW_DISCOVERY_WASM}
          ${WASM_HW_DISCOVERY_APP}
          hw-discovery
          hw_discovery_step
          16
          16
          2
          proc
          1
          ipc.basic
          0
  DEPENDS ${WASM_HW_DISCOVERY_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm hw-discovery as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_HELLO_C_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_HELLO_C_WASM}
          ${WASM_HELLO_C_APP}
          hello-c
          hello_c_step
          16
          16
          4
          -
          0
          ipc.basic
          0
  DEPENDS ${WASM_HELLO_C_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm hello-c as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_CHARDEV_CLIENT_WASM}
          ${WASM_CHARDEV_CLIENT_APP}
          chardev-client
          chardev_client_step
          16
          16
          4
          chardev
          1
          ipc.basic
          0
  DEPENDS ${WASM_CHARDEV_CLIENT_WASM} ${WASMOS_APP_PACKER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm chardev client as WASMOS-APP"
  VERBATIM
)

set(WASM_DRIVER_BLOBS
  ${WASM_CHARDEV_DRIVER_BLOB}
  ${WASM_CHARDEV_CLIENT_BLOB}
  ${WASM_ATA_APP}
  ${WASM_INIT_APP}
  ${WASM_CLI_APP}
  ${WASM_HW_DISCOVERY_APP}
  ${WASM_HELLO_C_APP}
  ${WASM_FAT_APP}
  ${WASM_CHARDEV_CLIENT_APP}
)
if (RUST_ENABLE)
  list(APPEND WASM_DRIVER_BLOBS ${WASM_HELLO_RUST_APP})
endif ()

if (ZIG_ENABLE)
  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (NOT ZIG_EXECUTABLE)
    message(WARNING "zig not found; Zig examples will be skipped.")
  else ()
    set(ZIG_AVAILABLE ON)
  endif ()
endif ()

if (AS_ENABLE)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  set(AS_HELLO_SRC ${EXAMPLES_AS_DIR}/hello/hello.ts)
  set(AS_HELLO_WASM ${BUILD_DIR}/hello_as.wasm)
  set(AS_HELLO_APP ${BUILD_DIR}/hello_as.wasmosapp)

  add_custom_command(
    OUTPUT ${AS_HELLO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${ASC_EXECUTABLE}
            ${AS_HELLO_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${AS_HELLO_WASM}
    DEPENDS ${AS_HELLO_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript hello module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${AS_HELLO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${AS_HELLO_WASM}
            ${AS_HELLO_APP}
            hello-as
            hello_step
            16
            16
            4
            -
            0
            ipc.basic
            0
    DEPENDS ${AS_HELLO_WASM} ${WASMOS_APP_PACKER}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing AssemblyScript hello as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(assemblyscript_examples DEPENDS ${AS_HELLO_APP})
endif ()

if (RUST_ENABLE)
  find_program(RUSTC_EXECUTABLE rustc)
  if (NOT RUSTC_EXECUTABLE)
    message(FATAL_ERROR "Rust compiler 'rustc' not found. Install Rust and add the wasm32-unknown-unknown target (rustup target add wasm32-unknown-unknown).")
  endif ()
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${RUSTC_EXECUTABLE}
            --target=wasm32-unknown-unknown
            -O
            -C opt-level=z
            -C panic=abort
            -C codegen-units=1
            -C link-arg=--no-entry
            -C link-arg=--export=hello_rust_step
            -C link-arg=--strip-all
            -C link-arg=--initial-memory=2097152
            -C link-arg=--max-memory=2097152
            --crate-type=cdylib
            ${WASM_HELLO_RUST_SRC}
            -o ${WASM_HELLO_RUST_WASM}
    DEPENDS ${WASM_HELLO_RUST_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building wasm hello-rust module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${WASM_HELLO_RUST_WASM}
            ${WASM_HELLO_RUST_APP}
            hello-rust
            hello_rust_step
            16
            16
            4
            -
            0
            ipc.basic
            0
    DEPENDS ${WASM_HELLO_RUST_WASM} ${WASMOS_APP_PACKER}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm hello-rust as WASMOS-APP"
    VERBATIM
  )
  add_custom_target(rust_examples DEPENDS ${WASM_HELLO_RUST_APP})
endif ()

if (ZIG_AVAILABLE)
  add_custom_command(
    OUTPUT ${WASM_HELLO_ZIG_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${ZIG_EXECUTABLE}
            build-exe
            -target wasm32-freestanding
            -O ReleaseSmall
            -fno-entry
            -fstrip
            --export=hello_zig_step
            -femit-bin=${WASM_HELLO_ZIG_WASM}
            ${WASM_HELLO_ZIG_SRC}
    DEPENDS ${WASM_HELLO_ZIG_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building Zig hello module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_HELLO_ZIG_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${WASM_HELLO_ZIG_WASM}
            ${WASM_HELLO_ZIG_APP}
            hello-zig
            hello_zig_step
            2
            2
            4
            -
            0
            ipc.basic
            0
    DEPENDS ${WASM_HELLO_ZIG_WASM} ${WASMOS_APP_PACKER}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing Zig hello as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(zig_examples DEPENDS ${WASM_HELLO_ZIG_APP})
  list(APPEND WASM_DRIVER_BLOBS ${WASM_HELLO_ZIG_APP})
endif ()

if (GO_ENABLE)
  find_program(TINYGO_EXECUTABLE tinygo)
  if (NOT TINYGO_EXECUTABLE)
    message(WARNING "TinyGo compiler 'tinygo' not found. Go example builds are disabled.")
  else ()
    set(GO_AVAILABLE ON)
  endif ()
endif ()
if (GO_AVAILABLE)
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${TINYGO_EXECUTABLE}
            build
            -target=wasm
            -opt=z
            -no-debug
            -panic=trap
            -scheduler=none
            -gc=leaking
            -o ${WASM_HELLO_GO_WASM}
            ${WASM_HELLO_GO_SRC}
    DEPENDS ${WASM_HELLO_GO_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building wasm hello-go module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${WASM_HELLO_GO_WASM}
            ${WASM_HELLO_GO_APP}
            hello-go
            hello_go_step
            16
            16
            4
            -
            0
            ipc.basic
            0
    DEPENDS ${WASM_HELLO_GO_WASM} ${WASMOS_APP_PACKER}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm hello-go as WASMOS-APP"
    VERBATIM
  )
  add_custom_target(go_examples DEPENDS ${WASM_HELLO_GO_APP})
  list(APPEND WASM_DRIVER_BLOBS ${WASM_HELLO_GO_APP})
endif ()

add_custom_target(wasm_driver_blobs DEPENDS ${WASM_DRIVER_BLOBS})
list(APPEND KERNEL_OBJS ${WASM_CHARDEV_DRIVER_BLOB} ${WASM_CHARDEV_CLIENT_BLOB})

# WAMR is vendored via git subtree at ${WAMR_DIR}.
option(WAMR_ENABLE "Enable WAMR runtime integration (requires linking WAMR)" ON)
option(WAMR_LINK "Link the WAMR runtime library" ON)
if (WAMR_ENABLE AND WAMR_LINK)
  set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -DWAMR_ENABLED=1 -I${WAMR_DIR}/core/iwasm/include)
endif ()

if (WAMR_LINK)
  set(WAMR_BUILD_PLATFORM "wasmos" CACHE STRING "WAMR build platform")
  set(WAMR_BUILD_TARGET "X86_64" CACHE STRING "WAMR build target")
  set(WAMR_BUILD_INTERP 1 CACHE BOOL "Enable WAMR interpreter")
  set(WAMR_BUILD_FAST_INTERP 0 CACHE BOOL "Disable WAMR fast interpreter")
  set(WAMR_BUILD_AOT 0 CACHE BOOL "Disable WAMR AOT")
  set(WAMR_BUILD_JIT 0 CACHE BOOL "Disable WAMR JIT")
  set(WAMR_BUILD_LIBC_BUILTIN 0 CACHE BOOL "Disable WAMR builtin libc")
  set(WAMR_BUILD_LIBC_WASI 0 CACHE BOOL "Disable WAMR WASI libc")
  set(WAMR_BUILD_LIBC_UVWASI 0 CACHE BOOL "Disable WAMR uvwasi libc")
  set(WAMR_DISABLE_APP_ENTRY 1 CACHE BOOL "Disable WAMR app entry helpers")
  set(WAMR_BUILD_LIB_PTHREAD 0 CACHE BOOL "Disable WAMR pthreads")
  set(WAMR_BUILD_THREAD_MGR 0 CACHE BOOL "Disable WAMR thread mgr")

  set(WAMR_ROOT_DIR ${WAMR_DIR})
  set(SHARED_PLATFORM_CONFIG ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/shared_platform.cmake)
  include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

  # runtime_lib.cmake configures required feature macros via add_definitions().
  # Propagate them into our custom WAMR object build commands.
  get_directory_property(WAMR_COMPILE_DEFINITIONS COMPILE_DEFINITIONS)
  set(WAMR_DEF_NAMES "")
  set(WAMR_DEF_VALUES "")
  foreach(def IN LISTS WAMR_COMPILE_DEFINITIONS)
    string(REGEX REPLACE "^([^=]+)=?.*$" "\\1" def_name "${def}")
    list(FIND WAMR_DEF_NAMES "${def_name}" def_idx)
    if (NOT def_idx EQUAL -1)
      list(REMOVE_AT WAMR_DEF_NAMES ${def_idx})
      list(REMOVE_AT WAMR_DEF_VALUES ${def_idx})
    endif ()
    list(APPEND WAMR_DEF_NAMES "${def_name}")
    list(APPEND WAMR_DEF_VALUES "${def}")
  endforeach()
  set(WAMR_DEF_FLAGS "")
  foreach(def IN LISTS WAMR_DEF_VALUES)
    list(APPEND WAMR_DEF_FLAGS -D${def})
  endforeach()

  if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT MACOS_SDK_PATH)
    execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE MACOS_SDK_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif ()

  find_program(LLVM_AR llvm-ar HINTS ${CLANG_BIN_DIR} /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
  if (NOT LLVM_AR)
    message(FATAL_ERROR "llvm-ar not found. Install LLVM and ensure llvm-ar is in PATH.")
  endif ()

  set(WAMR_INCLUDE_DIRS
    ${WAMR_ROOT_DIR}/core/iwasm/interpreter
    ${WAMR_ROOT_DIR}/core/iwasm/libraries/libc-builtin
    ${WAMR_ROOT_DIR}/core/iwasm/include
    ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos
    ${WAMR_ROOT_DIR}/core/shared/platform/include
    ${WAMR_ROOT_DIR}/core/shared/mem-alloc
    ${WAMR_ROOT_DIR}/core/iwasm/common
    ${WAMR_ROOT_DIR}/core/shared/utils
    ${WAMR_ROOT_DIR}/core/shared
  )

  set(WAMR_INC_FLAGS "")
  foreach(dir IN LISTS WAMR_INCLUDE_DIRS)
    list(APPEND WAMR_INC_FLAGS -I${dir})
  endforeach()
  if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND MACOS_SDK_PATH)
    list(APPEND WAMR_INC_FLAGS -isysroot ${MACOS_SDK_PATH} -isystem ${MACOS_SDK_PATH}/usr/include)
  endif ()

  set(WAMR_OBJ_DIR ${BUILD_DIR}/wamr_obj)
  set(WAMR_OBJS "")
  foreach(src IN LISTS WAMR_RUNTIME_LIB_SOURCE)
    string(REPLACE "/" "_" obj_name ${src})
    string(REPLACE "." "_" obj_name ${obj_name})
    set(obj ${WAMR_OBJ_DIR}/${obj_name}.o)
    add_custom_command(
      OUTPUT ${obj}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${WAMR_OBJ_DIR}
      COMMAND ${CLANG} ${CFLAGS_KERNEL} ${WAMR_DEF_FLAGS} -Wno-error -Wno-unused-command-line-argument -Wno-error=unused-parameter ${WAMR_INC_FLAGS}
              -c ${src} -o ${obj}
      DEPENDS ${src}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      VERBATIM
    )
    list(APPEND WAMR_OBJS ${obj})
  endforeach()

  set(WAMR_RUNTIME_LIB ${BUILD_DIR}/libwamr_runtime.a)
  add_custom_command(
    OUTPUT ${WAMR_RUNTIME_LIB}
    COMMAND ${LLVM_AR} rcs ${WAMR_RUNTIME_LIB} ${WAMR_OBJS}
    DEPENDS ${WAMR_OBJS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM
  )
  add_custom_target(wamr_runtime_lib DEPENDS ${WAMR_RUNTIME_LIB})
  set(KERNEL_LINK_LIBS ${KERNEL_LINK_LIBS} ${WAMR_RUNTIME_LIB})
endif ()

# Bootloader (UEFI PE/COFF)
add_custom_command(
  OUTPUT ${BUILD_DIR}/BOOTX64.EFI
  COMMAND ${CLANG} ${CFLAGS_EFI} -I${KERNEL_DIR}/include -I${BOOT_DIR}
          ${BOOT_SOURCES} -o ${BUILD_DIR}/BOOTX64.EFI ${LDFLAGS_EFI}
  DEPENDS ${BOOT_SOURCES} ${BOOT_DIR}/uefi.h ${BOOT_DIR}/elf.h ${KERNEL_DIR}/include/boot.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building UEFI bootloader"
  VERBATIM
)
add_custom_target(bootloader ALL DEPENDS ${BUILD_DIR}/BOOTX64.EFI)

# Kernel (ELF64)
add_custom_command(
  OUTPUT ${BUILD_DIR}/kernel.elf
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/entry.S -o ${BUILD_DIR}/entry.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/cpu_isr.S -o ${BUILD_DIR}/cpu_isr.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel.c -o ${BUILD_DIR}/kernel.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/cpu.c -o ${BUILD_DIR}/cpu.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory.c -o ${BUILD_DIR}/memory.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/paging.c -o ${BUILD_DIR}/paging.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/physmem.c -o ${BUILD_DIR}/physmem.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/ipc.c -o ${BUILD_DIR}/ipc.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process.c -o ${BUILD_DIR}/process.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/spinlock.c -o ${BUILD_DIR}/spinlock.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory_service.c -o ${BUILD_DIR}/memory_service.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/irq.c -o ${BUILD_DIR}/irq.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager.c -o ${BUILD_DIR}/process_manager.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasmos_app.c -o ${BUILD_DIR}/wasmos_app.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_driver.c -o ${BUILD_DIR}/wasm_driver.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wamr_context.c -o ${BUILD_DIR}/wamr_context.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_chardev.c -o ${BUILD_DIR}/wasm_chardev.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/serial.c -o ${BUILD_DIR}/serial.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${WASM_DIR}/wamr_runtime.c -o ${BUILD_DIR}/wamr_runtime.o
  COMMAND ${LLD_PATH} ${LDFLAGS_KERNEL} -o ${BUILD_DIR}/kernel.elf ${KERNEL_OBJS} ${KERNEL_LINK_LIBS}
  DEPENDS ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/arch/x86_64/linker.ld ${WASM_CHARDEV_DRIVER_BLOB} ${WASM_FAT_DRIVER_BLOB}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building kernel"
  VERBATIM
)
add_custom_target(kernel ALL DEPENDS ${BUILD_DIR}/kernel.elf)
if (WAMR_LINK)
  add_dependencies(kernel wamr_runtime_lib)
endif ()
add_dependencies(kernel wasm_driver_blobs)

# QEMU helper
if (AS_ENABLE)
  set(AS_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${AS_HELLO_APP} ${BUILD_DIR}/esp/apps/hello_as.wasmosapp
  )
endif ()
set(HELLO_C_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_C_APP} ${BUILD_DIR}/esp/apps/hello_c.wasmosapp
)
if (RUST_ENABLE)
  set(RUST_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_RUST_APP} ${BUILD_DIR}/esp/apps/hello_rust.wasmosapp
  )
endif ()
if (GO_AVAILABLE)
  set(GO_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_GO_APP} ${BUILD_DIR}/esp/apps/hello_go.wasmosapp
  )
endif ()
if (ZIG_AVAILABLE)
  set(ZIG_HELLO_COPY_CMD
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HELLO_ZIG_APP} ${BUILD_DIR}/esp/apps/hello_zig.wasmosapp
  )
endif ()

if (OVMF_VARS)
  add_custom_target(run-qemu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HW_DISCOVERY_APP} ${BUILD_DIR}/esp/system/services/hw_discovery.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wasmosapp
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
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
  if (AS_ENABLE)
    add_dependencies(run-qemu assemblyscript_examples)
  endif ()
  add_custom_target(run-qemu-test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HW_DISCOVERY_APP} ${BUILD_DIR}/esp/system/services/hw_discovery.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wasmosapp
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
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
    add_dependencies(run-qemu-test assemblyscript_examples)
  endif ()
  add_custom_target(run-qemu-cli-test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HW_DISCOVERY_APP} ${BUILD_DIR}/esp/system/services/hw_discovery.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wasmosapp
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${CMAKE_COMMAND} -E env
            WASMOS_OVMF_CODE=${OVMF_CODE}
            WASMOS_OVMF_VARS=${OVMF_VARS}
            WASMOS_ESP=${BUILD_DIR}/esp
            python3 -m unittest discover -s tests
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT \"Running QEMU CLI integration tests\"
    VERBATIM
  )
  if (AS_ENABLE)
    add_dependencies(run-qemu-cli-test assemblyscript_examples)
  endif ()
else ()
  add_custom_target(run-qemu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HW_DISCOVERY_APP} ${BUILD_DIR}/esp/system/services/hw_discovery.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wasmosapp
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -nographic -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
  if (AS_ENABLE)
    add_dependencies(run-qemu assemblyscript_examples)
  endif ()
  add_custom_target(run-qemu-test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HW_DISCOVERY_APP} ${BUILD_DIR}/esp/system/services/hw_discovery.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wasmosapp
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
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
    add_dependencies(run-qemu-test assemblyscript_examples)
  endif ()
  add_custom_target(run-qemu-cli-test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/services
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/system/drivers
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/apps/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_ATA_APP} ${BUILD_DIR}/esp/system/drivers/ata.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_FAT_APP} ${BUILD_DIR}/esp/system/drivers/fs_fat.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/apps/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_INIT_APP} ${BUILD_DIR}/esp/system/services/sysinit.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CLI_APP} ${BUILD_DIR}/esp/system/services/cli.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HW_DISCOVERY_APP} ${BUILD_DIR}/esp/system/services/hw_discovery.wasmosapp
    COMMAND ${CMAKE_COMMAND} -E copy ${WASM_CHARDEV_CLIENT_APP} ${BUILD_DIR}/esp/apps/chardev_client.wasmosapp
    ${AS_HELLO_COPY_CMD}
    ${HELLO_C_COPY_CMD}
    ${RUST_HELLO_COPY_CMD}
    ${GO_HELLO_COPY_CMD}
    ${ZIG_HELLO_COPY_CMD}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND ${CMAKE_COMMAND} -E env
            WASMOS_OVMF_CODE=${OVMF_CODE}
            WASMOS_ESP=${BUILD_DIR}/esp
            python3 -m unittest discover -s tests
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT \"Running QEMU CLI integration tests\"
    VERBATIM
  )
  if (AS_ENABLE)
    add_dependencies(run-qemu-cli-test assemblyscript_examples)
  endif ()
endif ()

if (RUST_ENABLE)
  add_dependencies(run-qemu rust_examples)
  add_dependencies(run-qemu-test rust_examples)
  add_dependencies(run-qemu-cli-test rust_examples)
endif ()

if (GO_AVAILABLE)
  add_dependencies(run-qemu go_examples)
  add_dependencies(run-qemu-test go_examples)
  add_dependencies(run-qemu-cli-test go_examples)
endif ()

if (ZIG_AVAILABLE)
  add_dependencies(run-qemu zig_examples)
  add_dependencies(run-qemu-test zig_examples)
  add_dependencies(run-qemu-cli-test zig_examples)
endif ()
