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
set(AS_HELLO_SRC ${EXAMPLES_AS_DIR}/hello/hello.ts)
set(AS_HELLO_WASM ${BUILD_DIR}/hello_as.wasm)
set(AS_HELLO_APP ${BUILD_DIR}/hello_as.wasmosapp)

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


add_executable(make_wasmos_app ${CMAKE_SOURCE_DIR}/scripts/make_wasmos_app.c)
target_compile_options(make_wasmos_app PRIVATE -O2)
set_target_properties(make_wasmos_app PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${BUILD_DIR}
)
set(WASMOS_APP_PACKER $<TARGET_FILE:make_wasmos_app>)

if (OVMF_VARS AND NOT EXISTS ${OVMF_VARS})
  message(WARNING "OVMF_VARS '${OVMF_VARS}' not found; continuing without vars.")
  set(OVMF_VARS "" CACHE FILEPATH "Path to OVMF_VARS.fd (optional)" FORCE)
endif ()

option(WAMR_ENABLE "Enable WAMR runtime integration (requires linking WAMR)" ON)
option(WAMR_LINK "Link the WAMR runtime library" ON)
if (WAMR_ENABLE AND WAMR_LINK)
  set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -DWAMR_ENABLED=1
      -I${WAMR_DIR}/core/iwasm/include
      -I${WAMR_DIR}/core/iwasm/common
      -I${WAMR_DIR}/core/shared/utils
      -I${WAMR_DIR}/core/shared/platform/include
      -I${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos)
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
if (WAMR_LINK)
  add_dependencies(kernel wamr_runtime_lib)
endif ()

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
            PYTHONDONTWRITEBYTECODE=1
            python3 -m unittest discover -s tests
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
endif ()
add_dependencies(run-qemu c_examples services drivers)
add_dependencies(run-qemu-test c_examples services drivers)
add_dependencies(run-qemu-cli-test c_examples services drivers)

if (TARGET assemblyscript_examples)
  add_dependencies(run-qemu assemblyscript_examples)
  add_dependencies(run-qemu-test assemblyscript_examples)
  add_dependencies(run-qemu-cli-test assemblyscript_examples)
endif ()
if (TARGET rust_examples)
  add_dependencies(run-qemu rust_examples)
  add_dependencies(run-qemu-test rust_examples)
  add_dependencies(run-qemu-cli-test rust_examples)
endif ()
if (TARGET go_examples)
  add_dependencies(run-qemu go_examples)
  add_dependencies(run-qemu-test go_examples)
  add_dependencies(run-qemu-cli-test go_examples)
endif ()
if (TARGET zig_examples)
  add_dependencies(run-qemu zig_examples)
  add_dependencies(run-qemu-test zig_examples)
  add_dependencies(run-qemu-cli-test zig_examples)
endif ()
