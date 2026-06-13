include(${CMAKE_SOURCE_DIR}/cmake/WasmosZigApp.cmake)

wasmos_add_zig_wasm_app(
  NAME        ps
  TARGET      ps_util
  SRC         ${WASM_PS_SRC}
  LIBC_SRC    ${LIBC_DIR}/zig/wasmos.zig
  OUTPUT_WASM ${WASM_PS_WASM}
  OUTPUT_APP  ${WASM_PS_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)
