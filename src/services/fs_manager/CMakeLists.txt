wasmos_add_wasm_c_app_target(fs_manager_app
  SOURCES
    ${WASM_FS_MANAGER_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_manager_path.c
  OUTPUT_WASM ${WASM_FS_MANAGER_WASM}
  OUTPUT_APP ${WASM_FS_MANAGER_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  BUILD_COMMENT "Building wasm fs-manager module"
  PACK_COMMENT "Packing wasm fs-manager as WASMOS-APP"
)
