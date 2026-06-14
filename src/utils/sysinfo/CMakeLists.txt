include(${CMAKE_SOURCE_DIR}/cmake/WasmosZigApp.cmake)

wasmos_add_zig_wasm_app(
  NAME        sysinfo
  TARGET      sysinfo_util
  SRC         ${WASM_SYSINFO_SRC}
  LIBC_SRC    ${LIBC_DIR}/zig/wasmos.zig
  OUTPUT_WASM ${WASM_SYSINFO_WASM}
  OUTPUT_APP  ${WASM_SYSINFO_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)
