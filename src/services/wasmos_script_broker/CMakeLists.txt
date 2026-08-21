wasmos_add_wasm_c_app_target(wamos_script_broker_app
  SOURCE      ${WASM_WAMOS_SCRIPT_BROKER_SRC}
  OUTPUT_WASM ${WASM_WAMOS_SCRIPT_BROKER_WASM}
  OUTPUT_APP  ${WASM_WAMOS_SCRIPT_BROKER_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm wamos-script broker service"
  PACK_COMMENT  "Packing wasm wamos-script broker as WASMOS-APP"
)
