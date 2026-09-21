# The formatter and the block client are both compiled in rather than shared as
# libraries: wfs_mkfs.c is also built into the host tool and the host unit suite,
# and wfs_block.c into the WFS driver, so each has one source and several builds.
wasmos_add_wasm_c_app_target(mkfs_wfs_util
  SOURCES     ${WASM_MKFS_WFS_SRC}
              ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs/wfs_mkfs.c
              ${DRIVER_WASM_DIR}/fs_wfs/wfs_block.c
              ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
              ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
  INCLUDES    ${CMAKE_CURRENT_SOURCE_DIR}
              ${CMAKE_SOURCE_DIR}/src/tools/mkfs_wfs
              ${DRIVER_WASM_DIR}/fs_wfs
  OUTPUT_WASM ${WASM_MKFS_WFS_WASM}
  OUTPUT_APP  ${WASM_MKFS_WFS_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm mkfs.wfs utility"
  PACK_COMMENT  "Packing wasm mkfs.wfs as WASMOS-APP"
)
