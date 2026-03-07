cmake_minimum_required(VERSION 3.20)
project(wasmos C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(BUILD_DIR ${CMAKE_BINARY_DIR})
set(BOOT_DIR ${CMAKE_SOURCE_DIR}/boot/efi)
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/kernel)
set(WASM_DIR ${CMAKE_SOURCE_DIR}/libs/wasm)
set(WAMR_DIR ${WASM_DIR}/wasm-micro-runtime)

set(CLANG clang CACHE STRING "C compiler for freestanding/UEFI builds")
set(LLD lld CACHE STRING "Linker (lld or ld.lld)")
set(LLD_LINK lld-link CACHE STRING "COFF linker (lld-link) for UEFI on macOS")
set(OBJCOPY llvm-objcopy CACHE STRING "Objcopy tool")
set(OVMF_CODE /usr/share/OVMF/OVMF_CODE.fd CACHE FILEPATH "Path to OVMF_CODE.fd")

set(LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /opt/homebrew/bin /usr/local/bin)

find_program(CLANG_PATH ${CLANG} HINTS ${LLVM_HINTS})
if (NOT CLANG_PATH)
  message(FATAL_ERROR "CLANG not found. Install LLVM clang and set -DCLANG=/path/to/llvm/bin/clang if needed.")
endif ()
set(CLANG ${CLANG_PATH} CACHE STRING "C compiler for freestanding/UEFI builds" FORCE)

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
set(LLD_HINTS ${CLANG_BIN_DIR} /opt/homebrew/opt/lld/bin /usr/local/opt/lld/bin /opt/homebrew/bin /usr/local/bin)

set(CFLAGS_COMMON -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -g)
set(CFLAGS_EFI ${CFLAGS_COMMON} -target x86_64-unknown-uefi -fshort-wchar -mno-red-zone -B${CLANG_BIN_DIR})
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -mno-red-zone -mcmodel=kernel -m64 -I${WASM_DIR})
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  find_program(LLD_LINK_PATH ${LLD_LINK} HINTS ${LLD_HINTS})
  if (LLD_LINK_PATH)
    get_filename_component(LLD_LINK_DIR ${LLD_LINK_PATH} DIRECTORY)
    if (NOT LLD_LINK_DIR STREQUAL CLANG_BIN_DIR)
      set(CFLAGS_EFI ${CFLAGS_EFI} -B${LLD_LINK_DIR})
    endif ()
    set(LDFLAGS_EFI -fuse-ld=lld-link -Wl,--subsystem,10 -Wl,--entry,efi_main)
  else ()
    find_program(LLD_PATH ${LLD} HINTS ${LLD_HINTS})
    if (NOT LLD_PATH)
      find_program(LLD_PATH ld.lld HINTS ${LLD_HINTS})
    endif ()
    if (NOT LLD_PATH)
      message(FATAL_ERROR "lld-link not found and lld/ld.lld not found. Install LLVM and ensure lld/lld-link/ld.lld is in PATH or set -DLLD_LINK=/path/to/lld-link -DLLD=/path/to/lld.")
    endif ()
    get_filename_component(LLD_DIR ${LLD_PATH} DIRECTORY)
    if (NOT LLD_DIR STREQUAL CLANG_BIN_DIR)
      set(CFLAGS_EFI ${CFLAGS_EFI} -B${LLD_DIR})
    endif ()
    set(LDFLAGS_EFI -fuse-ld=lld -Wl,-flavor,link -Wl,--subsystem,10 -Wl,--entry,efi_main)
  endif ()
else ()
  set(LDFLAGS_EFI -fuse-ld=lld -Wl,--subsystem,10 -Wl,--entry,efi_main)
endif ()
set(LDFLAGS_KERNEL -nostdlib -Wl,-T,${KERNEL_DIR}/arch/x86_64/linker.ld)

set(BOOT_SOURCES
  ${BOOT_DIR}/boot.c
)

set(KERNEL_SOURCES
  ${KERNEL_DIR}/arch/x86_64/entry.S
  ${KERNEL_DIR}/kernel.c
  ${WASM_DIR}/wamr_runtime.c
)

# Editor/indexer targets for CLion so sources are part of a CMake target.
add_library(bootloader_sources INTERFACE)
target_sources(bootloader_sources INTERFACE ${BOOT_SOURCES} ${BOOT_DIR}/uefi.h ${BOOT_DIR}/elf.h ${KERNEL_DIR}/include/boot.h)
target_include_directories(bootloader_sources INTERFACE ${BOOT_DIR} ${KERNEL_DIR}/include)

add_library(kernel_sources INTERFACE)
target_sources(kernel_sources INTERFACE ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${WASM_DIR}/wamr_runtime.h)
target_include_directories(kernel_sources INTERFACE ${KERNEL_DIR}/include ${WASM_DIR})

# WAMR is vendored via git subtree at ${WAMR_DIR}.
option(WAMR_ENABLE "Enable WAMR runtime integration (requires linking WAMR)" OFF)
if (WAMR_ENABLE)
  set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -DWAMR_ENABLED=1 -I${WAMR_DIR}/core/iwasm/include)
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
  COMMAND ${CLANG} ${CFLAGS_KERNEL} ${KERNEL_SOURCES}
          -o ${BUILD_DIR}/kernel.elf ${LDFLAGS_KERNEL}
  DEPENDS ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/arch/x86_64/linker.ld
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building kernel"
  VERBATIM
)
add_custom_target(kernel ALL DEPENDS ${BUILD_DIR}/kernel.elf)

# QEMU helper
add_custom_target(run-qemu
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
  COMMAND qemu-system-x86_64 -m 512M -bios ${OVMF_CODE}
          -drive format=raw,file=fat:rw:${BUILD_DIR}/esp -serial stdio
  DEPENDS bootloader kernel
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running in QEMU"
  VERBATIM
)
