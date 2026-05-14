set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

add_custom_command(
  OUTPUT ${WASM_CLI_APP_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
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
          ${WASM_CLI_APP_SRC}
          -o ${WASM_CLI_APP_WASM}
  DEPENDS ${WASM_CLI_APP_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm CLI module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_CLI_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_CLI_APP_WASM}
          ${WASM_CLI_APP}
          cli
          initialize
          16
          16
          4
          proc
          1
          1
          system.control
          0
          --entry-arg-bindings
          4
          proc.endpoint
          fs.endpoint
          vt.endpoint.or-neg1
          cli.tty.alloc
  DEPENDS ${WASM_CLI_APP_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm CLI as WASMOS-APP"
  VERBATIM
)

add_custom_target(cli_app DEPENDS ${WASM_CLI_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_CLI_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS cli_app)
