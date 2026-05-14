set(WASM_LIBC_C_SOURCES
  ${LIBC_DIR}/src/ctype.c
  ${LIBC_DIR}/src/stdlib.c
  ${LIBC_DIR}/src/string.c
  ${LIBC_DIR}/src/stdio.c
  ${LIBC_DIR}/src/unistd.c
)

set(WASM_DEVICE_MANAGER_MANIFEST_HEADER ${BUILD_DIR}/device_manager_manifest.h)

add_custom_command(
  OUTPUT ${WASM_DEVICE_MANAGER_MANIFEST_HEADER}
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${CMAKE_SOURCE_DIR}/configs/device_manager/ata.manifest
          -DOUTPUT=${WASM_DEVICE_MANAGER_MANIFEST_HEADER}
          -P ${CMAKE_SOURCE_DIR}/scripts/generate_device_manager_manifest.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/configs/device_manager/ata.manifest
          ${CMAKE_SOURCE_DIR}/scripts/generate_device_manager_manifest.cmake
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating device-manager ATA manifest header"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_DEVICE_MANAGER_WASM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} --target=wasm32 -Oz -nostdlib
          ${WASMOS_TRACE_DEFINE}
          -Wl,--no-entry -Wl,--strip-all
          -Wl,-z,stack-size=16384
          -Wl,--initial-memory=65536
          -Wl,--max-memory=65536
          -Wl,--allow-undefined
          -Wl,--export=initialize
          -I${LIBC_DIR}/include
          -I${BUILD_DIR}
          -I${DRIVER_WASM_DIR}/include
          ${WASM_LIBC_C_SOURCES}
          ${WASM_DEVICE_MANAGER_SRC}
          -o ${WASM_DEVICE_MANAGER_WASM}
  DEPENDS ${WASM_DEVICE_MANAGER_SRC} ${WASM_LIBC_C_SOURCES} ${LIBC_DIR}/include/ctype.h
          ${LIBC_DIR}/include/stdio.h ${LIBC_DIR}/include/stdlib.h ${LIBC_DIR}/include/string.h
          ${LIBC_DIR}/include/wasmos/api.h ${LIBC_DIR}/include/wasmos/ipc.h
          ${LIBC_DIR}/include/wasmos/imports.h ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
          ${WASM_DEVICE_MANAGER_MANIFEST_HEADER}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building wasm device-manager module"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_DEVICE_MANAGER_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${WASM_DEVICE_MANAGER_WASM}
          ${WASM_DEVICE_MANAGER_APP}
          device-manager
          initialize
          64
          64
          2
          proc
          1
          1
          ipc.basic
          0
          --entry-arg-bindings
          2
          proc.endpoint
          module.count
  DEPENDS ${WASM_DEVICE_MANAGER_WASM} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing wasm device-manager as WASMOS-APP"
  VERBATIM
)

add_custom_target(device_manager_app DEPENDS ${WASM_DEVICE_MANAGER_APP})

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_DEVICE_MANAGER_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS device_manager_app)
