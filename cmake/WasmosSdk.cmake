# WasmosSdk.cmake - stage a relocatable WASMOS SDK under ${BUILD_DIR}/wasmos-sdk.
#
# The SDK is the same toolchain the in-tree build uses, repackaged so a developer
# outside this repository can build a WASMOS application:
#
#   cmake --build build --target wasmos-sdk
#   export PATH=$PWD/build/wasmos-sdk/bin:$PATH
#   wasmos-clang hello.c -o hello        # -> hello.wap
#
# Structure (docs/toolchain.md documents each part):
#   bin/            driver + tool wrappers
#   libexec/wasmos/ make_wasmos_app, wasm_inspect.py
#   sysroot/include libc, libsys, libui headers + the generated ABI headers
#   sysroot/lib/<triple>  crt1.o, libc.a, libsys.a
#   share/          CMake integration, default manifest
#   wasmos-sdk.conf resolved tool paths and version, sourced by the wrappers
#
# Stage 1 keeps LLVM's own wasm32-unknown-unknown target and puts the WASMOS
# knowledge in the driver; WASMOS_SDK_TARGET is the name the SDK reports and
# WASMOS_SDK_TARGET_LLVM is what clang is actually given. They diverge until a
# native wasm32-unknown-wasmos triple exists.
set(WASMOS_SDK_DIR ${BUILD_DIR}/wasmos-sdk)
set(WASMOS_SDK_SYSROOT ${WASMOS_SDK_DIR}/sysroot)
set(WASMOS_SDK_TARGET wasm32-unknown-wasmos)
set(WASMOS_SDK_TARGET_LLVM wasm32-unknown-unknown)
set(WASMOS_SDK_LIBDIR ${WASMOS_SDK_SYSROOT}/lib/${WASMOS_SDK_TARGET})
set(WASMOS_SDK_VERSION 0.1.0-spike)

# llvm-ar is required, not optional: the sysroot archives it builds are what every
# WASM C target links against, so a tree without it cannot build a module at all.
# It ships with the same LLVM install that provides the clang, lld and
# llvm-objcopy this build already requires.
find_program(WASMOS_SDK_AR llvm-ar HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS})
if (NOT WASMOS_SDK_AR)
  message(FATAL_ERROR
    "llvm-ar not found. It builds the sysroot archives every WASM target links "
    "against; install LLVM's binutils or set -DWASMOS_SDK_AR=/path/to/llvm-ar.")
endif ()

foreach (_tool IN ITEMS nm ranlib strip objdump)
  find_program(WASMOS_SDK_${_tool} llvm-${_tool} HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS})
endforeach ()
find_program(WASMOS_SDK_WASMLD wasm-ld HINTS ${CLANG_BIN_DIR} ${LLVM_HINTS})

# Compile flags every sysroot object is built with. They must match what
# wasmos-clang passes for application sources, or an archive object and its
# caller would disagree about, for example, WASMOS_TRACE.
set(WASMOS_SDK_CFLAGS
  --target=${WASMOS_SDK_TARGET_LLVM}
  -Oz -nostdlib -ffreestanding -fno-builtin
  -D__wasmos__=1
  ${WASMOS_TRACE_DEFINE}
  -include ${LIBC_DIR}/include/wasmos_cast.h
  -I${CMAKE_SOURCE_DIR}/src/libui/include
  -I${LIBC_DIR}/include
  -I${LIBSYS_WASM_DIR}/include
  -I${DRIVER_WASM_DIR}/include
)

# wasmos_sdk_object(<out_var> <source> <object-name>)
# Compiles one source into ${BUILD_DIR}/sdk-obj/<object-name> and appends the
# object path to <out_var>.
function(wasmos_sdk_object out_var source obj_name)
  set(_obj ${BUILD_DIR}/sdk-obj/${obj_name})
  add_custom_command(
    OUTPUT ${_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/sdk-obj
    COMMAND ${CLANG} ${WASMOS_SDK_CFLAGS} -c ${source} -o ${_obj}
    DEPENDS ${source} ${WASMOS_LIBC_INCLUDE_DEPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "sdk: compiling ${obj_name}"
    VERBATIM
  )
  set(${out_var} ${${out_var}} ${_obj} PARENT_SCOPE)
endfunction()

# libc.a - everything under src/libc/src except startup.c, which becomes crt1.o.
# script.c (the .rc engine) is included even though only the CLI and the script
# executor use it: its own undefined symbols are all libc, so archive semantics
# leave it out of every module that does not reference it, and the alternative --
# shipping wasmos/script.h with no implementation behind it -- is a header that
# compiles and then fails to link.
set(WASMOS_SDK_LIBC_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/ipc_managed.c
  ${LIBC_DIR}/src/math.c
  ${LIBC_DIR}/src/script.c
  ${LIBC_DIR}/src/spawn_info.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/unistd.c
)
# libsys.a - the guest service runtime: coroutines, IPC futures, async entries.
set(WASMOS_SDK_LIBSYS_SOURCES
  ${LIBSYS_WASM_DIR}/coroutine_wasm.c
  ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
  ${LIBSYS_WASM_DIR}/service_runtime_wasm.c
)

set(WASMOS_SDK_LIBC_OBJS)
foreach (_src IN LISTS WASMOS_SDK_LIBC_SOURCES)
  get_filename_component(_we ${_src} NAME_WE)
  wasmos_sdk_object(WASMOS_SDK_LIBC_OBJS ${_src} libc_${_we}.o)
endforeach ()

set(WASMOS_SDK_LIBSYS_OBJS)
foreach (_src IN LISTS WASMOS_SDK_LIBSYS_SOURCES)
  get_filename_component(_we ${_src} NAME_WE)
  wasmos_sdk_object(WASMOS_SDK_LIBSYS_OBJS ${_src} libsys_${_we}.o)
endforeach ()

add_custom_command(
  OUTPUT ${WASMOS_SDK_LIBDIR}/libc.a
  COMMAND ${CMAKE_COMMAND} -E make_directory ${WASMOS_SDK_LIBDIR}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${WASMOS_SDK_LIBDIR}/libc.a
  COMMAND ${WASMOS_SDK_AR} rcs ${WASMOS_SDK_LIBDIR}/libc.a ${WASMOS_SDK_LIBC_OBJS}
  DEPENDS ${WASMOS_SDK_LIBC_OBJS}
  COMMENT "sdk: archiving libc.a"
  VERBATIM
)
add_custom_command(
  OUTPUT ${WASMOS_SDK_LIBDIR}/libsys.a
  COMMAND ${CMAKE_COMMAND} -E make_directory ${WASMOS_SDK_LIBDIR}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${WASMOS_SDK_LIBDIR}/libsys.a
  COMMAND ${WASMOS_SDK_AR} rcs ${WASMOS_SDK_LIBDIR}/libsys.a ${WASMOS_SDK_LIBSYS_OBJS}
  DEPENDS ${WASMOS_SDK_LIBSYS_OBJS}
  COMMENT "sdk: archiving libsys.a"
  VERBATIM
)

# crt1.o - the wasmos_main export the kernel calls, bridging to main().
add_custom_command(
  OUTPUT ${WASMOS_SDK_LIBDIR}/crt1.o
  COMMAND ${CMAKE_COMMAND} -E make_directory ${WASMOS_SDK_LIBDIR}
  COMMAND ${CLANG} ${WASMOS_SDK_CFLAGS} -c ${LIBC_DIR}/src/startup.c
          -o ${WASMOS_SDK_LIBDIR}/crt1.o
  DEPENDS ${LIBC_DIR}/src/startup.c ${WASMOS_LIBC_INCLUDE_DEPS}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "sdk: compiling crt1.o"
  VERBATIM
)

# The sysroot header tree. wasmos/api.h reaches the generated ABI headers by a
# repo-relative path (../../../../abi/generated/c/...) that no sysroot can
# reproduce, so the install rewrites those two includes to the sysroot layout and
# then asserts the rewrite happened. See docs/toolchain.md, "Why the sysroot
# rewrites one header".
set(WASMOS_SDK_STAGE_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/wasmos_sdk_stage.cmake)
set(WASMOS_SDK_STAMP ${BUILD_DIR}/sdk-obj/sysroot.stamp)
add_custom_command(
  OUTPUT ${WASMOS_SDK_STAMP}
  COMMAND ${CMAKE_COMMAND}
          -DSRC_DIR=${CMAKE_SOURCE_DIR}
          -DSDK_DIR=${WASMOS_SDK_DIR}
          -DSYSROOT=${WASMOS_SDK_SYSROOT}
          -DLIBC_DIR=${LIBC_DIR}
          -DLIBSYS_WASM_DIR=${LIBSYS_WASM_DIR}
          -DSDK_TARGET=${WASMOS_SDK_TARGET}
          -DSDK_TARGET_LLVM=${WASMOS_SDK_TARGET_LLVM}
          -DSDK_VERSION=${WASMOS_SDK_VERSION}
          -DSDK_CLANG=${CLANG}
          -DSDK_CLANGXX=${CMAKE_CXX_COMPILER}
          -DSDK_AR=${WASMOS_SDK_AR}
          -DSDK_NM=${WASMOS_SDK_nm}
          -DSDK_RANLIB=${WASMOS_SDK_ranlib}
          -DSDK_STRIP=${WASMOS_SDK_strip}
          -DSDK_OBJDUMP=${WASMOS_SDK_objdump}
          -DSDK_WASMLD=${WASMOS_SDK_WASMLD}
          -DPACKER=${WASMOS_APP_PACKER}
          -DSTAMP=${WASMOS_SDK_STAMP}
          -P ${WASMOS_SDK_STAGE_SCRIPT}
  DEPENDS ${WASMOS_SDK_STAGE_SCRIPT} ${WASMOS_APP_PACKER} make_wasmos_app
          ${CMAKE_SOURCE_DIR}/scripts/sdk/wasmos-clang
          ${CMAKE_SOURCE_DIR}/scripts/sdk/wasmos-clang++
          ${CMAKE_SOURCE_DIR}/scripts/sdk/wasmos-pack
          ${CMAKE_SOURCE_DIR}/scripts/sdk/wasmos-inspect
          ${CMAKE_SOURCE_DIR}/scripts/sdk/default-manifest.toml
          ${CMAKE_SOURCE_DIR}/scripts/sdk/WASMOSToolchain.cmake
          ${CMAKE_SOURCE_DIR}/scripts/sdk/Platform/WASMOS.cmake
          ${WASMOS_LIBC_INCLUDE_DEPS}
  COMMENT "sdk: staging sysroot, wrappers and CMake integration"
  VERBATIM
)

add_custom_target(wasmos-sdk
  DEPENDS ${WASMOS_SDK_LIBDIR}/libc.a
          ${WASMOS_SDK_LIBDIR}/libsys.a
          ${WASMOS_SDK_LIBDIR}/crt1.o
          ${WASMOS_SDK_STAMP}
)

# --- the SDK's own smoke app ----------------------------------------------
# Built with the staged driver and nothing else -- no manifest, no flags but -o --
# so the zero-configuration path is exercised by every build rather than only by
# hand. The .wap lands on the ESP as sdkhello.wap (FAT 8.3) and
# tests/test_sdk_hello.py runs it in the guest.
set(WASMOS_SDK_HELLO_SRC ${CMAKE_SOURCE_DIR}/examples/c/sdk_hello/hello.c)
set(WASMOS_SDK_HELLO_APP ${BUILD_DIR}/sdkhello.wap)
add_custom_command(
  OUTPUT ${WASMOS_SDK_HELLO_APP}
  COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-clang ${WASMOS_SDK_HELLO_SRC} -o ${WASMOS_SDK_HELLO_APP}
  DEPENDS ${WASMOS_SDK_HELLO_SRC}
          ${WASMOS_SDK_LIBDIR}/libc.a
          ${WASMOS_SDK_LIBDIR}/libsys.a
          ${WASMOS_SDK_LIBDIR}/crt1.o
          ${WASMOS_SDK_STAMP}
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "sdk: building sdk_hello with wasmos-clang"
  VERBATIM
)
add_custom_target(sdk_hello_app DEPENDS ${WASMOS_SDK_HELLO_APP})
# The app is built by the SDK driver rather than a wasmos_add_wasm_* helper, so it
# gets no compile_commands.json entry and would be invisible to clangd and the
# lint gate. Index it against the in-tree headers, which are the files the sysroot
# copies are made from, so the index does not depend on staging order.
wasmos_add_ide_c_target(sdk_hello_ide
  SOURCES ${WASMOS_SDK_HELLO_SRC}
  INCLUDES ${LIBC_DIR}/include ${LIBSYS_WASM_DIR}/include
)
add_dependencies(sdk_hello_app wasmos-sdk)
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS sdk_hello_app)

set(SDK_HELLO_COPY_CMD
  COMMAND ${CMAKE_COMMAND} -E copy ${WASMOS_SDK_HELLO_APP} ${BUILD_DIR}/esp/apps/sdkhello.wap
)
