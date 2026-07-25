wasmos_add_wasm_c_app_target(host_util
  SOURCE      ${WASM_HOST_SRC}
  OUTPUT_WASM ${WASM_HOST_WASM}
  OUTPUT_APP  ${WASM_HOST_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm host utility"
  PACK_COMMENT  "Packing wasm host as WASMOS-APP"
)
