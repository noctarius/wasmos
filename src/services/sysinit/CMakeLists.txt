wasmos_add_wasm_c_app_target(sysinit_app
  SOURCE ${WASM_INIT_APP_SRC}
  OUTPUT_WASM ${WASM_INIT_APP_WASM}
  OUTPUT_APP ${WASM_INIT_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 16384
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  BUILD_COMMENT "Building wasm init module"
  PACK_COMMENT "Packing wasm init as WASMOS-APP"
)
