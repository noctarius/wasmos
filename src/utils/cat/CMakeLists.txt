include(${CMAKE_SOURCE_DIR}/cmake/WasmosZigUtil.cmake)

wasmos_add_zig_util(
  NAME      cat
  SRC       ${WASM_CAT_SRC}
  LIBC_SRC  ${LIBC_DIR}/zig/wasmos.zig
  OUTPUT_WASM ${WASM_CAT_WASM}
  OUTPUT_APP  ${WASM_CAT_APP}
  MANIFEST  ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)
