set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
)

add_custom_command(
  OUTPUT ${WASM_FAT_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=initialize
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_FAT_DRIVER_SRC}
          -o ${WASM_FAT_DRIVER_WASM}
  DEPENDS ${WASM_FAT_DRIVER_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
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
          initialize
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
