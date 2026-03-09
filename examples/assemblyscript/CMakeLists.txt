if (AS_ENABLE)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${AS_HELLO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${ASC_EXECUTABLE}
            ${AS_HELLO_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${AS_HELLO_WASM}
    DEPENDS ${AS_HELLO_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript hello module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${AS_HELLO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${AS_HELLO_WASM}
            ${AS_HELLO_APP}
            hello-as
            hello_step
            16
            16
            4
            -
            0
            ipc.basic
            0
    DEPENDS ${AS_HELLO_WASM} make_wasmos_app
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing AssemblyScript hello as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(assemblyscript_examples DEPENDS ${AS_HELLO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${AS_HELLO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS assemblyscript_examples)
endif ()
