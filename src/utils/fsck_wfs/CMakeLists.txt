# The checker core is compiled into the utility rather than shared as a library:
# it is also compiled into the host unit suite, where it runs against volumes
# mkfs_wfs produced, so there is one source and two builds of it.
wasmos_add_wasm_c_app_target(fsck_wfs_util
  SOURCES     ${WASM_FSCK_WFS_SRC} ${WASM_FSCK_WFS_CORE}
              ${DRIVER_WASM_DIR}/fs_wfs/wfs_super.c
              ${DRIVER_WASM_DIR}/fs_wfs/wfs_crc32c.c
  INCLUDES    ${CMAKE_CURRENT_SOURCE_DIR} ${DRIVER_WASM_DIR}/fs_wfs
  OUTPUT_WASM ${WASM_FSCK_WFS_WASM}
  OUTPUT_APP  ${WASM_FSCK_WFS_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm fsck.wfs utility"
  PACK_COMMENT  "Packing wasm fsck.wfs as WASMOS-APP"
)
