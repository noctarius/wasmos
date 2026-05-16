wasmos_add_wasm_c_app_target(fs_init_app
  SOURCE ${WASM_INITFS_DRIVER_SRC}
  OUTPUT_WASM ${WASM_INITFS_DRIVER_WASM}
  OUTPUT_APP ${WASM_INITFS_APP}
  MANIFEST ${DRIVER_WASM_DIR}/fs_init/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  NO_BUILTIN
  BUILD_COMMENT "Building wasm initfs driver module"
  PACK_COMMENT "Packing wasm initfs driver as WASMOS-APP"
)
