wasmos_add_wasm_c_app_target(mkdir_util
  SOURCE      ${WASM_MKDIR_SRC}
  OUTPUT_WASM ${WASM_MKDIR_WASM}
  OUTPUT_APP  ${WASM_MKDIR_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm mkdir utility"
  PACK_COMMENT  "Packing wasm mkdir as WASMOS-APP"
)
