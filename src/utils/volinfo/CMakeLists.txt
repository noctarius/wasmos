wasmos_add_wasm_c_app_target(volinfo_util
  SOURCE      ${WASM_VOLINFO_SRC}
  OUTPUT_WASM ${WASM_VOLINFO_WASM}
  OUTPUT_APP  ${WASM_VOLINFO_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm volinfo utility"
  PACK_COMMENT  "Packing wasm volinfo as WASMOS-APP"
)
