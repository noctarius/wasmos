if (AS_ENABLE)
  set(WASM_SERIAL_STAGE_DIR ${BUILD_DIR}/serial_src)
  set(WASM_SERIAL_ENTRY_SRC ${WASM_SERIAL_STAGE_DIR}/serial.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_SERIAL_DRIVER_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_SERIAL_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_SERIAL_DRIVER_SRC} ${WASM_SERIAL_STAGE_DIR}/serial.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/assemblyscript/wasmos.ts ${WASM_SERIAL_STAGE_DIR}/wasmos.ts
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_SERIAL_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_SERIAL_DRIVER_WASM}
    DEPENDS ${WASM_SERIAL_DRIVER_SRC} ${LIBC_DIR}/assemblyscript/wasmos.ts
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript serial driver module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_SERIAL_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${DRIVER_WASM_DIR}/serial/linker.manifest
            --in ${WASM_SERIAL_DRIVER_WASM}
            --out ${WASM_SERIAL_APP}
    DEPENDS ${WASM_SERIAL_DRIVER_WASM} make_wasmos_app
            ${DRIVER_WASM_DIR}/serial/linker.manifest
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing serial driver as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(serial_app DEPENDS ${WASM_SERIAL_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_SERIAL_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS serial_app)
endif ()
