set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

add_custom_command(
  OUTPUT ${WASM_HELLO_C_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_HELLO_C_SRC}
          -o ${WASM_HELLO_C_WASM}
  DEPENDS ${WASM_HELLO_C_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
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
          main
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
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_CHARDEV_CLIENT_SRC}
          -o ${WASM_CHARDEV_CLIENT_WASM}
  DEPENDS ${WASM_CHARDEV_CLIENT_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
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
          main
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

add_custom_command(
  OUTPUT ${WASM_CHARDEV_PREEMPT_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_CHARDEV_PREEMPT_SRC}
          -o ${WASM_CHARDEV_PREEMPT_WASM}
  DEPENDS ${WASM_CHARDEV_PREEMPT_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm chardev preempt module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_PREEMPT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_CHARDEV_PREEMPT_WASM}
          ${WASM_CHARDEV_PREEMPT_APP}
          chardev-preempt
          main
          16
          16
          4
          chardev
          1
          ipc.basic
          0
  DEPENDS ${WASM_CHARDEV_PREEMPT_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm chardev preempt as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_INIT_SMOKE_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_INIT_SMOKE_SRC}
          -o ${WASM_INIT_SMOKE_WASM}
  DEPENDS ${WASM_INIT_SMOKE_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm init smoke module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_INIT_SMOKE_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_INIT_SMOKE_WASM}
          ${WASM_INIT_SMOKE_APP}
          init-smoke
          main
          16
          16
          4
          -
          0
          -
          0
  DEPENDS ${WASM_INIT_SMOKE_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm init smoke as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_NATIVE_CALL_SMOKE_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_NATIVE_CALL_SMOKE_SRC}
          -o ${WASM_NATIVE_CALL_SMOKE_WASM}
  DEPENDS ${WASM_NATIVE_CALL_SMOKE_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm native call smoke module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_NATIVE_CALL_SMOKE_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_NATIVE_CALL_SMOKE_WASM}
          ${WASM_NATIVE_CALL_SMOKE_APP}
          native-call-smoke
          main
          16
          16
          4
          -
          0
          -
          0
  DEPENDS ${WASM_NATIVE_CALL_SMOKE_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm native call smoke as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FS_OPEN_SMOKE_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_FS_OPEN_SMOKE_SRC}
          -o ${WASM_FS_OPEN_SMOKE_WASM}
  DEPENDS ${WASM_FS_OPEN_SMOKE_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/unistd.h
          ${LIBC_DIR}/include/fcntl.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm fs open smoke module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FS_OPEN_SMOKE_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_FS_OPEN_SMOKE_WASM}
          ${WASM_FS_OPEN_SMOKE_APP}
          fs-open-smoke
          main
          16
          16
          4
          -
          0
          ipc.basic
          0
  DEPENDS ${WASM_FS_OPEN_SMOKE_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm fs open smoke as WASMOS-APP"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_NATIVE_CALL_MIN_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib -ffreestanding -fno-builtin
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=main
          -I${LIBC_DIR}/include
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_NATIVE_CALL_MIN_SRC}
          -o ${WASM_NATIVE_CALL_MIN_WASM}
  DEPENDS ${WASM_NATIVE_CALL_MIN_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/stdio.h
          ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h ${LIBC_DIR}/include/wasmos/imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm native call min module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_NATIVE_CALL_MIN_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_NATIVE_CALL_MIN_WASM}
          ${WASM_NATIVE_CALL_MIN_APP}
          native-call-min
          main
          16
          16
          4
          -
          0
          -
          0
  DEPENDS ${WASM_NATIVE_CALL_MIN_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm native call min as WASMOS-APP"
  VERBATIM
)

add_custom_target(c_examples DEPENDS ${WASM_HELLO_C_APP} ${WASM_CHARDEV_CLIENT_APP} ${WASM_CHARDEV_PREEMPT_APP} ${WASM_INIT_SMOKE_APP} ${WASM_FS_OPEN_SMOKE_APP} ${WASM_NATIVE_CALL_MIN_APP} ${WASM_NATIVE_CALL_SMOKE_APP})
add_custom_target(chardev_client_blob DEPENDS ${WASM_CHARDEV_CLIENT_BLOB})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_C_APP} ${WASM_CHARDEV_CLIENT_APP} ${WASM_CHARDEV_PREEMPT_APP} ${WASM_INIT_SMOKE_APP} ${WASM_FS_OPEN_SMOKE_APP} ${WASM_NATIVE_CALL_MIN_APP} ${WASM_NATIVE_CALL_SMOKE_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS c_examples)
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${WASM_CHARDEV_CLIENT_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS chardev_client_blob)
