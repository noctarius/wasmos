wasmos_add_wasm_c_app_target(virtio_net_app
  SOURCE ${WASM_VIRTIO_NET_DRIVER_SRC}
  OUTPUT_WASM ${WASM_VIRTIO_NET_DRIVER_WASM}
  OUTPUT_APP ${WASM_VIRTIO_NET_APP}
  MANIFEST ${DRIVER_WASM_DIR}/virtio_net/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  # region_alloc places the virtqueue-ring window above the driver's live data
  # (map_auto low guard = 2 MiB), so give the module 4 MiB of linear memory like
  # the shmem/DMA-capable apps; MAX must be >= region window offset+size or
  # warp_linmem_place_phys returns NO_WINDOW.
  INITIAL_MEMORY 4194304
  MAX_MEMORY 4194304
  NO_BUILTIN
  BUILD_COMMENT "Building wasm virtio-net driver module"
  PACK_COMMENT "Packing wasm virtio-net driver as WASMOS-APP"
)
