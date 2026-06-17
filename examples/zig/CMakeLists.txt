include(${CMAKE_SOURCE_DIR}/cmake/WasmosZigApp.cmake)

# Signal availability for the ZIG_HELLO_COPY_CMD guard in the top-level
# CMakeLists after wasmos_add_zig_wasm_app resolves ZIG_EXECUTABLE.
if (ZIG_ENABLE)
  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (ZIG_EXECUTABLE)
    set(ZIG_AVAILABLE ON)
    set(ZIG_AVAILABLE ON PARENT_SCOPE)
  endif ()
endif ()

wasmos_add_zig_wasm_app(
  NAME        hello_zig
  TARGET      zig_examples
  SRC         ${WASM_HELLO_ZIG_SRC}
  LIBC_SRC    ${LIBC_DIR}/zig/wasmos.zig
  OUTPUT_WASM ${WASM_HELLO_ZIG_WASM}
  OUTPUT_APP  ${WASM_HELLO_ZIG_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)

wasmos_add_zig_wasm_app(
  NAME        calculator
  TARGET      zig_calculator
  SRC         ${WASM_CALCULATOR_SRC}
  LIBC_SRC    ${LIBC_DIR}/zig/wasmos.zig
  OUTPUT_WASM ${WASM_CALCULATOR_WASM}
  OUTPUT_APP  ${WASM_CALCULATOR_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/calculator/linker.metadata
  EXTRA_SRCS
    ${CMAKE_SOURCE_DIR}/src/libui/zig/libui.zig
    ${CMAKE_SOURCE_DIR}/src/libui/zig/libui_shim.c
  INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src/libc/zig/compat
    ${CMAKE_SOURCE_DIR}/src/libui/include
    ${CMAKE_SOURCE_DIR}/src/libc/include
    ${CMAKE_SOURCE_DIR}/src/drivers/include
)
