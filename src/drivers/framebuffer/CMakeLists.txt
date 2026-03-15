if (AS_ENABLE)
  set(WASM_FRAMEBUFFER_STAGE_DIR ${BUILD_DIR}/framebuffer_src)
  set(WASM_FRAMEBUFFER_ENTRY_SRC ${WASM_FRAMEBUFFER_STAGE_DIR}/framebuffer.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_FRAMEBUFFER_DRIVER_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_FRAMEBUFFER_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_FRAMEBUFFER_DRIVER_SRC} ${WASM_FRAMEBUFFER_STAGE_DIR}/framebuffer.ts
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_FRAMEBUFFER_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_FRAMEBUFFER_DRIVER_WASM}
    DEPENDS ${WASM_FRAMEBUFFER_DRIVER_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript framebuffer driver module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_FRAMEBUFFER_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${WASM_FRAMEBUFFER_DRIVER_WASM}
            ${WASM_FRAMEBUFFER_APP}
            framebuffer
            initialize
            16
            16
            1
            -
            0
            ipc.basic
            0
    DEPENDS ${WASM_FRAMEBUFFER_DRIVER_WASM} make_wasmos_app
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing framebuffer driver as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(framebuffer_app DEPENDS ${WASM_FRAMEBUFFER_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_FRAMEBUFFER_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS framebuffer_app)
endif ()
