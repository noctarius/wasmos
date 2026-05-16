wasmos_add_wasm_c_app_target(fs_fat_app
  SOURCE ${WASM_FAT_DRIVER_SRC}
  OUTPUT_WASM ${WASM_FAT_DRIVER_WASM}
  OUTPUT_APP ${WASM_FAT_APP}
  MANIFEST ${DRIVER_WASM_DIR}/fs_fat/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  NO_BUILTIN
  BUILD_COMMENT "Building wasm FAT driver module"
  PACK_COMMENT "Packing wasm FAT as WASMOS-APP"
)
