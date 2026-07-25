wasmos_add_wasm_c_app_target(curl_util
  SOURCE      ${WASM_CURL_SRC}
  OUTPUT_WASM ${WASM_CURL_WASM}
  OUTPUT_APP  ${WASM_CURL_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  # Room for the TCP socket rings overlaid into linear memory (above the 2 MiB
  # scan floor / heap) via wasmos_xfer_buffer_map.
  INITIAL_MEMORY 4194304
  MAX_MEMORY     4194304
  BUILD_COMMENT "Building wasm curl utility"
  PACK_COMMENT  "Packing wasm curl as WASMOS-APP"
)
