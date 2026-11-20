if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
  unset(ZIG_EXECUTABLE CACHE)
  find_program(ZIG_EXECUTABLE zig HINTS
    ${CLANG_BIN_DIR}
    $ENV{HOME}/bin
    $ENV{HOME}/.local/bin
  )
endif ()

if (NOT ZIG_EXECUTABLE)
  message(FATAL_ERROR "zig is required to build native gfx-compositor service")
endif ()

set(GFX_COMPOSITOR_ZIG_OBJ ${BUILD_DIR}/gfx_compositor_zig.o)
set(GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ ${BUILD_DIR}/gfx_compositor_libsys_exports.o)
set(GFX_COMPOSITOR_LIBC_STRING_OBJ ${BUILD_DIR}/gfx_compositor_libc_string.o)
set(GFX_COMPOSITOR_LIBC_CTYPE_OBJ ${BUILD_DIR}/gfx_compositor_libc_ctype.o)
set(GFX_COMPOSITOR_ELF ${WASM_GFX_COMPOSITOR_WASM})

# Generate a tiny Zig module that exposes WASMOS_TRACE as a comptime bool.
# This lets gfx_compositor.zig read GFX_TRACE from the build system rather
# than having it hardcoded, matching how C apps use -DWASMOS_TRACE.
if (WASMOS_TRACE)
  set(GFX_TRACE_ZIG_BOOL true)
else ()
  set(GFX_TRACE_ZIG_BOOL false)
endif ()
set(GFX_COMPOSITOR_BUILD_OPTIONS_ZIG ${BUILD_DIR}/gfx_compositor_build_options.zig)
file(WRITE "${GFX_COMPOSITOR_BUILD_OPTIONS_ZIG}"
     "pub const gfx_trace: bool = ${GFX_TRACE_ZIG_BOOL};\n")

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_ZIG_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${ZIG_EXECUTABLE}
          build-obj
          -target x86_64-freestanding
          -O ReleaseSmall
          -fstrip
          --dep libsys
          --dep build_options
          -I ${CMAKE_SOURCE_DIR}
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -Mroot=${WASM_GFX_COMPOSITOR_SRC}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -Mlibsys=${LIBSYS_NATIVE_DIR}/zig/libsys.zig
          -Mbuild_options=${GFX_COMPOSITOR_BUILD_OPTIONS_ZIG}
          -femit-bin=${GFX_COMPOSITOR_ZIG_OBJ}
  DEPENDS ${WASM_GFX_COMPOSITOR_SRC}
          ${GFX_COMPOSITOR_BUILD_OPTIONS_ZIG}
          ${LIBSYS_NATIVE_DIR}/zig/libsys.zig
          ${LIBSYS_NATIVE_DIR}/zig/c_abi.zig
          ${CMAKE_CURRENT_SOURCE_DIR}/gfx_compositor_imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
          ${LIBC_DIR}/include/wasmos/gfx_ipc.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building native Zig gfx-compositor object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -c ${LIBSYS_NATIVE_DIR}/zig/libsys_native.c
          -o ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/zig/libsys_native.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building gfx-compositor libsys exports object"
  VERBATIM
)

# Service-name watches (svc_watch_native.c) and the future runtime their await
# form resolves through (coroutine_native.c + its context switch).
set(GFX_COMPOSITOR_LIBSYS_RUNTIME_OBJS)
set(GFX_COMPOSITOR_COROUTINE_SWITCH_OBJ ${BUILD_DIR}/gfx_compositor_coroutine_native_x86_64.o)
foreach (gfx_libsys_src svc_watch_native coroutine_native)
  set(gfx_libsys_obj ${BUILD_DIR}/gfx_compositor_${gfx_libsys_src}.o)
  list(APPEND GFX_COMPOSITOR_LIBSYS_RUNTIME_OBJS ${gfx_libsys_obj})
  add_custom_command(
    OUTPUT ${gfx_libsys_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CLANG}
            -target ${KERNEL_TARGET_TRIPLE}
            -ffreestanding
            -fno-stack-protector
            -fno-pic
            -fno-pie
            -fno-builtin
            -nostdlib
            -O2
            -I ${DRIVER_WASM_DIR}/include
            -I ${LIBC_DIR}/include
            -I ${LIBSYS_NATIVE_DIR}/include
            -c ${LIBSYS_NATIVE_DIR}/${gfx_libsys_src}.c
            -o ${gfx_libsys_obj}
    DEPENDS ${LIBSYS_NATIVE_DIR}/${gfx_libsys_src}.c
            ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native.h
            ${LIBSYS_NATIVE_DIR}/include/wasmos/coroutine_native.h
            ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
            ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building gfx-compositor ${gfx_libsys_src} object"
    VERBATIM
  )
endforeach ()

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_COROUTINE_SWITCH_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} -target ${KERNEL_TARGET_TRIPLE}
          -c ${LIBSYS_NATIVE_DIR}/coroutine_native_x86_64.S
          -o ${GFX_COMPOSITOR_COROUTINE_SWITCH_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/coroutine_native_x86_64.S
          ${LIBSYS_NATIVE_DIR}/include/wasmos/coroutine_native.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building gfx-compositor coroutine context switch object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_LIBC_CTYPE_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/ctype.c
          -o ${GFX_COMPOSITOR_LIBC_CTYPE_OBJ}
  DEPENDS ${LIBC_DIR}/src/ctype.c ${LIBC_DIR}/include/ctype.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building gfx-compositor libc ctype object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_LIBC_STRING_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/string.c
          -o ${GFX_COMPOSITOR_LIBC_STRING_OBJ}
  DEPENDS ${LIBC_DIR}/src/string.c ${LIBC_DIR}/include/string.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building gfx-compositor libc string object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_ELF}
  COMMAND ${LLD_PATH}
          -e initialize
          -static
          ${GFX_COMPOSITOR_ZIG_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
          ${GFX_COMPOSITOR_LIBC_CTYPE_OBJ}
          ${GFX_COMPOSITOR_LIBC_STRING_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_RUNTIME_OBJS}
          ${GFX_COMPOSITOR_COROUTINE_SWITCH_OBJ}
          -o ${GFX_COMPOSITOR_ELF}
  DEPENDS ${GFX_COMPOSITOR_ZIG_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
          ${GFX_COMPOSITOR_LIBC_CTYPE_OBJ}
          ${GFX_COMPOSITOR_LIBC_STRING_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_RUNTIME_OBJS}
          ${GFX_COMPOSITOR_COROUTINE_SWITCH_OBJ}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Linking native Zig gfx-compositor service"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_GFX_COMPOSITOR_APP}
  COMMAND ${WASMOS_APP_PACKER}
          --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
          --in ${GFX_COMPOSITOR_ELF}
          --out ${WASM_GFX_COMPOSITOR_APP}
  DEPENDS ${GFX_COMPOSITOR_ELF} make_wasmos_app ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing native Zig gfx-compositor as WASMOS-APP"
  VERBATIM
)

add_custom_target(gfx_compositor_app
  DEPENDS ${WASM_GFX_COMPOSITOR_APP}
  SOURCES ${WASM_GFX_COMPOSITOR_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_GFX_COMPOSITOR_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS gfx_compositor_app)
