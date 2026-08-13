# chardev-server: a normal WASM driver packed as a WASMOS-APP and loaded from
# initfs via device-manager (see initfs.toml + devmgr rules), like every other
# driver.
wasmos_add_wasm_c_app_target(chardev_server_app
  SOURCE ${WASM_CHARDEV_DRIVER_SRC}
  OUTPUT_WASM ${WASM_CHARDEV_DRIVER_WASM}
  OUTPUT_APP ${WASM_CHARDEV_SERVER_APP}
  MANIFEST ${DRIVER_WASM_DIR}/chardev/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  NO_BUILTIN
  BUILD_COMMENT "Building wasm chardev driver module"
  PACK_COMMENT "Packing wasm chardev as WASMOS-APP"
)
