wasmos_add_wasm_c_app_target(ip_util
  SOURCE      ${WASM_IP_SRC}
  OUTPUT_WASM ${WASM_IP_WASM}
  OUTPUT_APP  ${WASM_IP_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm ip utility"
  PACK_COMMENT  "Packing wasm ip as WASMOS-APP"
)
