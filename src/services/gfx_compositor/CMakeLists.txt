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
set(GFX_COMPOSITOR_ELF ${WASM_GFX_COMPOSITOR_WASM})

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_ZIG_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${ZIG_EXECUTABLE}
          build-obj
          -target x86_64-freestanding
          -O ReleaseSmall
          -fstrip
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -femit-bin=${GFX_COMPOSITOR_ZIG_OBJ}
          ${WASM_GFX_COMPOSITOR_SRC}
  DEPENDS ${WASM_GFX_COMPOSITOR_SRC}
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
          ${LIBC_DIR}/include/wasmos/gfx_ipc.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building native Zig gfx-compositor object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${GFX_COMPOSITOR_ELF}
  COMMAND ${LLD_PATH}
          -e initialize
          -static
          ${GFX_COMPOSITOR_ZIG_OBJ}
          -o ${GFX_COMPOSITOR_ELF}
  DEPENDS ${GFX_COMPOSITOR_ZIG_OBJ}
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
