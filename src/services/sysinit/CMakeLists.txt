add_custom_command(
  OUTPUT ${WASM_INIT_APP_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=16384
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=initialize
          -I${DRIVER_WASM_DIR}/include
          ${WASM_INIT_APP_SRC}
          -o ${WASM_INIT_APP_WASM}
  DEPENDS ${WASM_INIT_APP_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm init module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_INIT_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_INIT_APP_WASM}
          ${WASM_INIT_APP}
          sysinit
          initialize
          64
          64
          4
          proc
          1
          ipc.basic
          0
  DEPENDS ${WASM_INIT_APP_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm init as WASMOS-APP"
  VERBATIM
)

add_custom_target(sysinit_app DEPENDS ${WASM_INIT_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_INIT_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS sysinit_app)
