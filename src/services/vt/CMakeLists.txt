wasmos_add_wasm_c_app_target(vt_app
  SOURCE ${WASM_VT_APP_SRC}
  OUTPUT_WASM ${WASM_VT_APP_WASM}
  OUTPUT_APP ${WASM_VT_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 131072
  MAX_MEMORY 1048576
  BUILD_COMMENT "Building wasm vt module"
  PACK_COMMENT "Packing wasm vt as WASMOS-APP"
)
