if (AS_ENABLE)
  set(WASM_KEYBOARD_STAGE_DIR ${BUILD_DIR}/keyboard_src)
  set(WASM_KEYBOARD_ENTRY_SRC ${WASM_KEYBOARD_STAGE_DIR}/keyboard.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_KEYBOARD_DRIVER_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_KEYBOARD_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_KEYBOARD_DRIVER_SRC} ${WASM_KEYBOARD_STAGE_DIR}/keyboard.ts
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_KEYBOARD_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_KEYBOARD_DRIVER_WASM}
    DEPENDS ${WASM_KEYBOARD_DRIVER_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript keyboard driver module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_KEYBOARD_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${DRIVER_WASM_DIR}/keyboard/linker.manifest
            --in ${WASM_KEYBOARD_DRIVER_WASM}
            --out ${WASM_KEYBOARD_APP}
    DEPENDS ${WASM_KEYBOARD_DRIVER_WASM} make_wasmos_app
            ${DRIVER_WASM_DIR}/keyboard/linker.manifest
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing keyboard driver as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(keyboard_app DEPENDS ${WASM_KEYBOARD_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_KEYBOARD_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS keyboard_app)
endif ()
