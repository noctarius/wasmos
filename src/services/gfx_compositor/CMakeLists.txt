wasmos_add_wasm_c_app_target(gfx_compositor_app
  SOURCE ${WASM_GFX_COMPOSITOR_SRC}
  OUTPUT_WASM ${WASM_GFX_COMPOSITOR_WASM}
  OUTPUT_APP ${WASM_GFX_COMPOSITOR_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 8192
  INITIAL_MEMORY 65536
  MAX_MEMORY 262144
  BUILD_COMMENT "Building wasm gfx-compositor module"
  PACK_COMMENT "Packing wasm gfx-compositor as WASMOS-APP"
)
