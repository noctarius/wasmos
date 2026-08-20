wasmos_add_wasm_c_app_target(virtio_net_app
  SOURCE ${WASM_VIRTIO_NET_DRIVER_SRC}
  OUTPUT_WASM ${WASM_VIRTIO_NET_DRIVER_WASM}
  OUTPUT_APP ${WASM_VIRTIO_NET_APP}
  MANIFEST ${DRIVER_WASM_DIR}/virtio_net/linker.metadata
  EXPORT initialize
  NO_BUILTIN
  BUILD_COMMENT "Building wasm virtio-net driver module"
  PACK_COMMENT "Packing wasm virtio-net driver as WASMOS-APP"
)
