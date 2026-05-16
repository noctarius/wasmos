set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

add_custom_command(
  OUTPUT ${WASM_PCI_BUS_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=16384
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=initialize
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_PCI_BUS_SRC}
          -o ${WASM_PCI_BUS_WASM}
  DEPENDS ${WASM_PCI_BUS_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm pci-bus module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_PCI_BUS_APP}
  COMMAND ${WASMOS_APP_PACKER}
          --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
          --in ${WASM_PCI_BUS_WASM}
          --out ${WASM_PCI_BUS_APP}
  DEPENDS ${WASM_PCI_BUS_WASM} make_wasmos_app
          ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm pci-bus as WASMOS-APP"
  VERBATIM
)

add_custom_target(pci_bus_app
  DEPENDS ${WASM_PCI_BUS_APP}
  SOURCES ${WASM_PCI_BUS_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_PCI_BUS_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS pci_bus_app)

wasmos_add_ide_c_target(pci_bus_ide
  SOURCES ${WASM_PCI_BUS_SRC}
  INCLUDES ${LIBC_DIR}/include ${DRIVER_WASM_DIR}/include
)
