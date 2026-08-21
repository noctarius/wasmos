wasmos_add_wasm_c_app_target(curl_util
  SOURCE      ${WASM_CURL_SRC}
  OUTPUT_WASM ${WASM_CURL_WASM}
  OUTPUT_APP  ${WASM_CURL_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  # curl carries several KB of URL/request/receive buffers in main() and builds a
  # net_socket_open_descriptor (which now includes a 256-byte SNI field for TLS
  # hostname verification) on the stack in the net.h connect helper. The 4 KB
  # default C stack is too small for that combination, so give it 16 KB.
  # Room for the TCP socket rings overlaid into linear memory (above the 2 MiB
  # scan floor / heap) via wasmos_xfer_buffer_map.
  BUILD_COMMENT "Building wasm curl utility"
  PACK_COMMENT  "Packing wasm curl as WASMOS-APP"
)
