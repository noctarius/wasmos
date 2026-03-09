add_custom_command(
  OUTPUT ${WASM_FAT_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=fat_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_FAT_DRIVER_SRC}
          -o ${WASM_FAT_DRIVER_WASM}
  DEPENDS ${WASM_FAT_DRIVER_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm FAT driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FAT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_FAT_DRIVER_WASM}
          ${WASM_FAT_APP}
          fs-fat
          fat_step
          16
          16
          2
          block
          1
          ipc.basic
          0
  DEPENDS ${WASM_FAT_DRIVER_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm FAT as WASMOS-APP"
  VERBATIM
)

add_custom_target(fs_fat_app DEPENDS ${WASM_FAT_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_FAT_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS fs_fat_app)
