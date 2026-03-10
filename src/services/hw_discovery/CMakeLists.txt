add_custom_command(
  OUTPUT ${WASM_HW_DISCOVERY_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=4096
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=initialize
          -Wl,--export=dispatch
          -Wl,--export=hw_discovery_step
          -I${DRIVER_WASM_DIR}/include
          ${WASM_HW_DISCOVERY_SRC}
          -o ${WASM_HW_DISCOVERY_WASM}
  DEPENDS ${WASM_HW_DISCOVERY_SRC} ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm hw-discovery module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_HW_DISCOVERY_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_HW_DISCOVERY_WASM}
          ${WASM_HW_DISCOVERY_APP}
          hw-discovery
          hw_discovery_step
          16
          16
          2
          proc
          1
          ipc.basic
          0
  DEPENDS ${WASM_HW_DISCOVERY_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm hw-discovery as WASMOS-APP"
  VERBATIM
)

add_custom_target(hw_discovery_app DEPENDS ${WASM_HW_DISCOVERY_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HW_DISCOVERY_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS hw_discovery_app)
