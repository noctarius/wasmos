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
set(GFX_COMPOSITOR_ELF ${WASM_GFX_COMPOSITOR_WASM})

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_ZIG_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${ZIG_EXECUTABLE}
          build-obj
          -target x86_64-freestanding
          -O ReleaseSmall
          -fstrip
          --dep libsys
          -I ${CMAKE_SOURCE_DIR}
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -Mroot=${WASM_GFX_COMPOSITOR_SRC}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -Mlibsys=${LIBSYS_NATIVE_DIR}/zig/libsys.zig
          -femit-bin=${GFX_COMPOSITOR_ZIG_OBJ}
  DEPENDS ${WASM_GFX_COMPOSITOR_SRC}
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
          -c ${LIBSYS_NATIVE_DIR}/zig/libsys_exports.c
          -o ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/zig/libsys_exports.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native_exports.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building gfx-compositor libsys exports object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_ELF}
  COMMAND ${LLD_PATH}
          -e initialize
          -static
          ${GFX_COMPOSITOR_ZIG_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
          -o ${GFX_COMPOSITOR_ELF}
  DEPENDS ${GFX_COMPOSITOR_ZIG_OBJ}
          ${GFX_COMPOSITOR_LIBSYS_EXPORTS_OBJ}
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
