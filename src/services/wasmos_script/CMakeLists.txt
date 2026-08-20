wasmos_add_wasm_c_app_target(wamos_script_app
  SOURCES     ${WASM_WAMOS_SCRIPT_SRC}
  OUTPUT_WASM ${WASM_WAMOS_SCRIPT_WASM}
  OUTPUT_APP  ${WASM_WAMOS_SCRIPT_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm wamos-script executor module"
  PACK_COMMENT  "Packing wasm wamos-script executor as WASMOS-APP"
)
