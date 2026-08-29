wasmos_add_wasm_c_app_target(fs_wfs_app
  SOURCES ${WASM_WFS_DRIVER_SRC}
  OUTPUT_WASM ${WASM_WFS_DRIVER_WASM}
  OUTPUT_APP ${WASM_WFS_APP}
  MANIFEST ${DRIVER_WASM_DIR}/fs_wfs/linker.metadata
  BUILD_COMMENT "Building wasm WFS driver module"
  PACK_COMMENT "Packing wasm WFS as WASMOS-APP"
)
