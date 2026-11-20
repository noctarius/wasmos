wasmos_add_wasm_c_app_target(supervisor_app
  SOURCES ${WASM_SUPERVISOR_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/supervisor_unit.c
  OUTPUT_WASM ${WASM_SUPERVISOR_WASM}
  OUTPUT_APP ${WASM_SUPERVISOR_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm supervisor service"
  PACK_COMMENT "Packing wasm supervisor as WASMOS-APP"
)
