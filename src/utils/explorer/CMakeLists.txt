wasmos_add_wasm_c_app_target(explorer_util
  SOURCE      ${WASM_EXPLORER_SRC}
  OUTPUT_WASM ${WASM_EXPLORER_WASM}
  OUTPUT_APP  ${WASM_EXPLORER_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT  "Building wasm explorer utility"
  PACK_COMMENT   "Packing wasm explorer as WASMOS-APP"
)
