wasmos_add_wasm_c_app_target(fs_fat_app
  SOURCES ${WASM_FAT_DRIVER_SRC}
  OUTPUT_WASM ${WASM_FAT_DRIVER_WASM}
  OUTPUT_APP ${WASM_FAT_APP}
  MANIFEST ${DRIVER_WASM_DIR}/fs_fat/linker.metadata
  EXPORT initialize
  STACK_SIZE 8192
  INITIAL_MEMORY 262144
  MAX_MEMORY 262144
  NO_BUILTIN
  BUILD_COMMENT "Building wasm FAT driver module"
  PACK_COMMENT "Packing wasm FAT as WASMOS-APP"
)
