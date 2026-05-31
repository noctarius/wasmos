wasmos_add_wasm_c_app_target(virtio_serial_app
  SOURCE ${WASM_VIRTIO_SERIAL_DRIVER_SRC}
  OUTPUT_WASM ${WASM_VIRTIO_SERIAL_DRIVER_WASM}
  OUTPUT_APP ${WASM_VIRTIO_SERIAL_APP}
  MANIFEST ${DRIVER_WASM_DIR}/virtio_serial/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  NO_BUILTIN
  BUILD_COMMENT "Building wasm virtio-serial driver module"
  PACK_COMMENT "Packing wasm virtio-serial driver as WASMOS-APP"
)
