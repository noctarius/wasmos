wasmos_add_wasm_c_app_target(mount_util
  SOURCE      ${WASM_MOUNT_SRC}
  OUTPUT_WASM ${WASM_MOUNT_WASM}
  OUTPUT_APP  ${WASM_MOUNT_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm mount utility"
  PACK_COMMENT  "Packing wasm mount as WASMOS-APP"
)
