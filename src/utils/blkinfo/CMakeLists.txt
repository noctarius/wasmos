wasmos_add_wasm_c_app_target(blkinfo_util
  SOURCE      ${WASM_BLKINFO_SRC}
  OUTPUT_WASM ${WASM_BLKINFO_WASM}
  OUTPUT_APP  ${WASM_BLKINFO_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm blkinfo utility"
  PACK_COMMENT  "Packing wasm blkinfo as WASMOS-APP"
)
