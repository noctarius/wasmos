add_custom_command(
  OUTPUT ${WASM_HELLO_C_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=hello_c_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_HELLO_C_SRC}
          -o ${WASM_HELLO_C_WASM}
  DEPENDS ${WASM_HELLO_C_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm hello-c module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_HELLO_C_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_HELLO_C_WASM}
          ${WASM_HELLO_C_APP}
          hello-c
          hello_c_step
          16
          16
          4
          -
          0
          ipc.basic
          0
  DEPENDS ${WASM_HELLO_C_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm hello-c as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=chardev_client_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_CHARDEV_CLIENT_SRC}
          -o ${WASM_CHARDEV_CLIENT_WASM}
  DEPENDS ${WASM_CHARDEV_CLIENT_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm chardev client module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_BLOB}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          chardev_client.wasm chardev_client_blob.o
  DEPENDS ${WASM_CHARDEV_CLIENT_WASM}
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Embedding wasm chardev client module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_CHARDEV_CLIENT_WASM}
          ${WASM_CHARDEV_CLIENT_APP}
          chardev-client
          chardev_client_step
          16
          16
          4
          chardev
          1
          ipc.basic
          0
  DEPENDS ${WASM_CHARDEV_CLIENT_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm chardev client as WASMOS-APP"
  VERBATIM
)

add_custom_target(c_examples DEPENDS ${WASM_HELLO_C_APP} ${WASM_CHARDEV_CLIENT_APP})
add_custom_target(chardev_client_blob DEPENDS ${WASM_CHARDEV_CLIENT_BLOB})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_C_APP} ${WASM_CHARDEV_CLIENT_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS c_examples)
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${WASM_CHARDEV_CLIENT_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS chardev_client_blob)
