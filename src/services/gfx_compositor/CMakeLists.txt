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
          -o ${GFX_COMPOSITOR_ELF}
  DEPENDS ${GFX_COMPOSITOR_ZIG_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
          ${GFX_COMPOSITOR_LIBC_CTYPE_OBJ}
          ${GFX_COMPOSITOR_LIBC_STRING_OBJ}
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
