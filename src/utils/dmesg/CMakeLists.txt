wasmos_add_wasm_c_app_target(dmesg_util
  SOURCE      ${WASM_DMESG_SRC}
  OUTPUT_WASM ${WASM_DMESG_WASM}
  OUTPUT_APP  ${WASM_DMESG_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm dmesg utility"
  PACK_COMMENT  "Packing wasm dmesg as WASMOS-APP"
)
