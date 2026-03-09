add_custom_command(
  OUTPUT ${WASM_ATA_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=ata_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_ATA_DRIVER_SRC}
          -o ${WASM_ATA_DRIVER_WASM}
  DEPENDS ${WASM_ATA_DRIVER_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
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
          ata_step
          16
          16
          1
          -
          0
          ipc.basic
          0
  DEPENDS ${WASM_ATA_DRIVER_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm ATA as WASMOS-APP"
  VERBATIM
)

add_custom_target(ata_app DEPENDS ${WASM_ATA_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_ATA_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ata_app)
