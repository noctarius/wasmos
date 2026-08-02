wasmos_add_wasm_c_app_target(vt_app
  SOURCE ${WASM_VT_APP_SRC}
  OUTPUT_WASM ${WASM_VT_APP_WASM}
  OUTPUT_APP ${WASM_VT_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 4096
  # Initial memory sized to hold the VT's active memory (cell grids) plus the
  # klog ring's low overlay window (phase 4), so the window falls inside the
  # module's declared initial linear memory — the kernel commit-probe then only
  # faults pages in, never extends the module (extending under WARP throws).
  INITIAL_MEMORY 1048576
  # Raised from 1 MiB: the klog ring (phase 4) is overlaid into the VT's linear
  # memory via xfer_buffer_map.  Under WARP the overlay window is placed above
  # the VT's active memory (past the 2 MiB low-guard), so the WASM linear memory
  # must be able to grow past 2 MiB plus the window plus scan guards to host it.
  MAX_MEMORY 4194304
  BUILD_COMMENT "Building wasm vt module"
  PACK_COMMENT "Packing wasm vt as WASMOS-APP"
)
