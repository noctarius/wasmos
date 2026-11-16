wasmos_add_wasm_c_app_target(logger_app
  SOURCES ${WASM_LOGGER_SRC}
  OUTPUT_WASM ${WASM_LOGGER_WASM}
  OUTPUT_APP ${WASM_LOGGER_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  ASYNC_SERVICE
  BUILD_COMMENT "Building wasm logger service"
  PACK_COMMENT "Packing wasm logger as WASMOS-APP"
)
