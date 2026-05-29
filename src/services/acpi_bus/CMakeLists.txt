wasmos_add_wasm_c_app_target(acpi_bus_app
  SOURCE ${WASM_ACPI_BUS_SRC}
  OUTPUT_WASM ${WASM_ACPI_BUS_WASM}
  OUTPUT_APP ${WASM_ACPI_BUS_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 16384
  INITIAL_MEMORY 131072
  MAX_MEMORY 131072
  BUILD_COMMENT "Building wasm acpi-bus module"
  PACK_COMMENT "Packing wasm acpi-bus as WASMOS-APP"
)
