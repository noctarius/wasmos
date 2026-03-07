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
set(LLD lld CACHE STRING "Linker")
set(OBJCOPY llvm-objcopy CACHE STRING "Objcopy tool")
set(OVMF_CODE /usr/share/OVMF/OVMF_CODE.fd CACHE FILEPATH "Path to OVMF_CODE.fd")

set(CFLAGS_COMMON -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -g)
set(CFLAGS_EFI ${CFLAGS_COMMON} -target x86_64-unknown-uefi -fshort-wchar -mno-red-zone)
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -mno-red-zone -mcmodel=kernel -m64 -I${WASM_DIR})
set(LDFLAGS_EFI -fuse-ld=lld -Wl,--subsystem,10 -Wl,--entry,efi_main)
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
