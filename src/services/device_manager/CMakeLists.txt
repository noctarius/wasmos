wasmos_add_wasm_c_app_target(device_manager_app
  SOURCE ${WASM_DEVICE_MANAGER_SRC}
  OUTPUT_WASM ${WASM_DEVICE_MANAGER_WASM}
  OUTPUT_APP ${WASM_DEVICE_MANAGER_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 16384
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  BUILD_COMMENT "Building wasm device-manager module"
  PACK_COMMENT "Packing wasm device-manager as WASMOS-APP"
)
