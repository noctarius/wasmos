if (AS_ENABLE)
  set(AS_HELLO_LIBC_SRC ${LIBC_DIR}/assemblyscript/wasmos.ts)
  set(AS_HELLO_RUNTIME_SRC ${LIBC_DIR}/assemblyscript/runtime.ts)
  set(AS_HELLO_STAGE_DIR ${BUILD_DIR}/assemblyscript_hello_src)
  set(AS_HELLO_ENTRY_SRC ${AS_HELLO_STAGE_DIR}/entry.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${AS_HELLO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${AS_HELLO_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AS_HELLO_SRC} ${AS_HELLO_STAGE_DIR}/app.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AS_HELLO_LIBC_SRC} ${AS_HELLO_STAGE_DIR}/wasmos.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AS_HELLO_RUNTIME_SRC} ${AS_HELLO_ENTRY_SRC}
    COMMAND ${ASC_EXECUTABLE}
            ${AS_HELLO_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${AS_HELLO_WASM}
    DEPENDS ${AS_HELLO_SRC} ${AS_HELLO_LIBC_SRC} ${AS_HELLO_RUNTIME_SRC}
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
            wasmos_main
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
