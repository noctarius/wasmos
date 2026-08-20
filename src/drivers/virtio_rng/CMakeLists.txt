wasmos_add_wasm_c_app_target(virtio_rng_app
  SOURCE ${WASM_VIRTIO_RNG_DRIVER_SRC}
  OUTPUT_WASM ${WASM_VIRTIO_RNG_DRIVER_WASM}
  OUTPUT_APP ${WASM_VIRTIO_RNG_APP}
  MANIFEST ${DRIVER_WASM_DIR}/virtio_rng/linker.metadata
  EXPORT initialize
  # region_alloc places the request-queue ring + entropy pool window above the
  # driver's live data (map_auto low guard = 2 MiB), so give the module 4 MiB of
  # linear memory like the other DMA-capable drivers; MAX must be >= region
  # window offset+size or warp_linmem_place_phys returns NO_WINDOW.
  NO_BUILTIN
  BUILD_COMMENT "Building wasm virtio-rng driver module"
  PACK_COMMENT "Packing wasm virtio-rng driver as WASMOS-APP"
)
