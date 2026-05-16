add_custom_command(
  OUTPUT ${WASM_CHARDEV_DRIVER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -fno-builtin
          ${WASMOS_TRACE_DEFINE}
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
  DEPENDS ${WASM_CHARDEV_DRIVER_SRC} ${WASM_LIBC_C_SOURCES} ${WASMOS_LIBC_INCLUDE_DEPS}
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

wasmos_add_ide_c_target(chardev_ide
  SOURCES ${WASM_CHARDEV_DRIVER_SRC}
  INCLUDES ${LIBC_DIR}/include ${DRIVER_WASM_DIR}/include
)

set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${WASM_CHARDEV_DRIVER_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS chardev_driver_blob)
