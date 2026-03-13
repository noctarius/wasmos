set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--export=initialize
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_CHARDEV_DRIVER_SRC}
          -o ${WASM_CHARDEV_DRIVER_WASM}
  DEPENDS ${WASM_CHARDEV_DRIVER_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm chardev driver module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_DRIVER_BLOB}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          chardev_server.wasm chardev_server_blob.o
  DEPENDS ${WASM_CHARDEV_DRIVER_WASM}
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Embedding wasm chardev driver module"
  VERBATIM
)

add_custom_target(chardev_driver_blob DEPENDS ${WASM_CHARDEV_DRIVER_BLOB})

set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${WASM_CHARDEV_DRIVER_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS chardev_driver_blob)
