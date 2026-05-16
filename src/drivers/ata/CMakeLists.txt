wasmos_add_wasm_c_app_target(ata_app
  SOURCE ${WASM_ATA_DRIVER_SRC}
  OUTPUT_WASM ${WASM_ATA_DRIVER_WASM}
  OUTPUT_APP ${WASM_ATA_APP}
  MANIFEST ${DRIVER_WASM_DIR}/ata/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  NO_BUILTIN
  BUILD_COMMENT "Building wasm ATA driver module"
  PACK_COMMENT "Packing wasm ATA as WASMOS-APP"
)
