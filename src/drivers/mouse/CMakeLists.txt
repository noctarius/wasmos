if (AS_ENABLE)
  set(WASM_MOUSE_STAGE_DIR ${BUILD_DIR}/mouse_src)
  set(WASM_MOUSE_ENTRY_SRC ${WASM_MOUSE_STAGE_DIR}/mouse.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_MOUSE_DRIVER_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_MOUSE_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_MOUSE_DRIVER_SRC} ${WASM_MOUSE_STAGE_DIR}/mouse.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/assemblyscript/wasmos.ts ${WASM_MOUSE_STAGE_DIR}/wasmos.ts
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_MOUSE_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_MOUSE_DRIVER_WASM}
    DEPENDS ${WASM_MOUSE_DRIVER_SRC} ${LIBC_DIR}/assemblyscript/wasmos.ts
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript mouse driver module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_MOUSE_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${DRIVER_WASM_DIR}/mouse/linker.metadata
            --in ${WASM_MOUSE_DRIVER_WASM}
            --out ${WASM_MOUSE_APP}
    DEPENDS ${WASM_MOUSE_DRIVER_WASM} make_wasmos_app
            ${DRIVER_WASM_DIR}/mouse/linker.metadata
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing mouse driver as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(mouse_app
    DEPENDS ${WASM_MOUSE_APP}
    SOURCES ${WASM_MOUSE_DRIVER_SRC} ${DRIVER_WASM_DIR}/mouse/linker.metadata
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_MOUSE_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS mouse_app)
endif ()
