wasmos_add_wasm_c_app_target(pci_bus_app
  SOURCE ${WASM_PCI_BUS_SRC}
  OUTPUT_WASM ${WASM_PCI_BUS_WASM}
  OUTPUT_APP ${WASM_PCI_BUS_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 16384
  INITIAL_MEMORY 65536
  MAX_MEMORY 65536
  BUILD_COMMENT "Building wasm pci-bus module"
  PACK_COMMENT "Packing wasm pci-bus as WASMOS-APP"
)
