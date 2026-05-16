set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

add_custom_command(
  OUTPUT ${WASM_INITFS_DRIVER_WASM}
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
          ${WASM_INITFS_DRIVER_SRC}
          -o ${WASM_INITFS_DRIVER_WASM}
  DEPENDS ${WASM_INITFS_DRIVER_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm initfs driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_INITFS_APP}
  COMMAND ${WASMOS_APP_PACKER}
          --manifest ${DRIVER_WASM_DIR}/fs_init/linker.metadata
          --in ${WASM_INITFS_DRIVER_WASM}
          --out ${WASM_INITFS_APP}
  DEPENDS ${WASM_INITFS_DRIVER_WASM} make_wasmos_app
          ${DRIVER_WASM_DIR}/fs_init/linker.metadata
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm initfs driver as WASMOS-APP"
  VERBATIM
)

add_custom_target(fs_init_app
  DEPENDS ${WASM_INITFS_APP}
  SOURCES ${WASM_INITFS_DRIVER_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_INITFS_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS fs_init_app)

wasmos_add_ide_c_target(fs_init_ide
  SOURCES ${WASM_INITFS_DRIVER_SRC}
  INCLUDES ${LIBC_DIR}/include ${DRIVER_WASM_DIR}/include
)
