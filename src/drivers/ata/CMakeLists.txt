set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

add_custom_command(
  OUTPUT ${WASM_ATA_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=initialize
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_ATA_DRIVER_SRC}
          -o ${WASM_ATA_DRIVER_WASM}
  DEPENDS ${WASM_ATA_DRIVER_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm ATA driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_ATA_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_ATA_DRIVER_WASM}
          ${WASM_ATA_APP}
          ata
          initialize
          16
          16
          33
          -
          0
          2
          io.port
          0
          irq.route
          0
          --entry-arg-bindings
          1
          proc.endpoint
          --driver-match
          0x01
          any
          any
          any
          any
          0x01F0
          0x03F7
  DEPENDS ${WASM_ATA_DRIVER_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm ATA as WASMOS-APP"
  VERBATIM
)

add_custom_target(ata_app DEPENDS ${WASM_ATA_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_ATA_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ata_app)
