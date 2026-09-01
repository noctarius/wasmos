wasmos_add_wasm_c_app_target(umount_util
  SOURCE      ${WASM_UMOUNT_SRC}
  OUTPUT_WASM ${WASM_UMOUNT_WASM}
  OUTPUT_APP  ${WASM_UMOUNT_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm umount utility"
  PACK_COMMENT  "Packing wasm umount as WASMOS-APP"
)
