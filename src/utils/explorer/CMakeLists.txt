wasmos_add_wasm_c_app_target(explorer_util
  SOURCE      ${WASM_EXPLORER_SRC}
  OUTPUT_WASM ${WASM_EXPLORER_WASM}
  OUTPUT_APP  ${WASM_EXPLORER_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  INITIAL_MEMORY 4194304
  MAX_MEMORY     4194304
  BUILD_COMMENT  "Building wasm explorer utility"
  PACK_COMMENT   "Packing wasm explorer as WASMOS-APP"
)
