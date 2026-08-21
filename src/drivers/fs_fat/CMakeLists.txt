wasmos_add_wasm_c_app_target(fs_fat_app
  SOURCES ${WASM_FAT_DRIVER_SRC}
  OUTPUT_WASM ${WASM_FAT_DRIVER_WASM}
  OUTPUT_APP ${WASM_FAT_APP}
  MANIFEST ${DRIVER_WASM_DIR}/fs_fat/linker.metadata
  BUILD_COMMENT "Building wasm FAT driver module"
  PACK_COMMENT "Packing wasm FAT as WASMOS-APP"
)
