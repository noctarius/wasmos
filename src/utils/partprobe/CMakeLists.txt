wasmos_add_wasm_c_app_target(partprobe_util
  SOURCE      ${WASM_PARTPROBE_SRC}
  OUTPUT_WASM ${WASM_PARTPROBE_WASM}
  OUTPUT_APP  ${WASM_PARTPROBE_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm partprobe utility"
  PACK_COMMENT  "Packing wasm partprobe as WASMOS-APP"
)
