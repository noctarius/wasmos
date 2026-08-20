wasmos_add_wasm_c_app_target(sysinit_app
  SOURCES ${WASM_INIT_APP_SRC}
  OUTPUT_WASM ${WASM_INIT_APP_WASM}
  OUTPUT_APP ${WASM_INIT_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  BUILD_COMMENT "Building wasm init module"
  PACK_COMMENT "Packing wasm init as WASMOS-APP"
)
