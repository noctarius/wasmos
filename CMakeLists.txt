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
set(BOOT_DIR ${CMAKE_SOURCE_DIR}/boot/efi)
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/kernel)
set(WASM_DIR ${CMAKE_SOURCE_DIR}/libs/wasm)
set(WAMR_DIR ${WASM_DIR}/wasm-micro-runtime)

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
set(CFLAGS_KERNEL ${CFLAGS_COMMON} -target ${KERNEL_TARGET_TRIPLE} -mno-red-zone -mcmodel=kernel -m64 -I${WASM_DIR} -I${KERNEL_DIR}/include -B${LLD_DIR})
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
target_sources(kernel_sources INTERFACE ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/include/cpu.h ${KERNEL_DIR}/include/ipc.h ${KERNEL_DIR}/include/paging.h ${KERNEL_DIR}/include/process.h ${KERNEL_DIR}/include/spinlock.h ${KERNEL_DIR}/include/wasm_chardev.h ${WASM_DIR}/wamr_runtime.h)
target_include_directories(kernel_sources INTERFACE ${KERNEL_DIR}/include ${WASM_DIR})

# IDE aggregation targets: helps CLion mark files as part of the CMake project
# even though the real build uses custom commands/targets.
add_custom_target(bootloader_ide SOURCES
  ${BOOT_SOURCES}
  ${BOOT_DIR}/uefi.h
  ${BOOT_DIR}/elf.h
  ${KERNEL_DIR}/include/boot.h
)

add_custom_target(kernel_ide SOURCES
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${WASM_DIR}/wamr_runtime.h
)

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
  set(SHARED_PLATFORM_CONFIG ${CMAKE_SOURCE_DIR}/platform/wasmos/shared_platform.cmake)
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
    ${CMAKE_SOURCE_DIR}/platform/wasmos
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
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wamr_context.c -o ${BUILD_DIR}/wamr_context.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_chardev.c -o ${BUILD_DIR}/wasm_chardev.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/serial.c -o ${BUILD_DIR}/serial.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${WASM_DIR}/wamr_runtime.c -o ${BUILD_DIR}/wamr_runtime.o
  COMMAND ${LLD_PATH} ${LDFLAGS_KERNEL} -o ${BUILD_DIR}/kernel.elf ${KERNEL_OBJS} ${KERNEL_LINK_LIBS}
  DEPENDS ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/arch/x86_64/linker.ld
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building kernel"
  VERBATIM
)
add_custom_target(kernel ALL DEPENDS ${BUILD_DIR}/kernel.elf)
if (WAMR_LINK)
  add_dependencies(kernel wamr_runtime_lib)
endif ()

# QEMU helper
if (OVMF_VARS)
  add_custom_target(run-qemu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
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
else ()
  add_custom_target(run-qemu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/BOOTX64.EFI ${BUILD_DIR}/esp/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/kernel.elf ${BUILD_DIR}/esp/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/startup.nsh ${BUILD_DIR}/esp/startup.nsh
    COMMAND qemu-system-x86_64 -m 512M -nographic -serial mon:stdio
            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
            -drive format=raw,file=fat:rw:${BUILD_DIR}/esp
    DEPENDS bootloader kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running in QEMU"
    VERBATIM
  )
endif ()
