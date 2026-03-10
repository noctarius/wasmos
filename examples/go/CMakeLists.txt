if (GO_ENABLE)
  find_program(TINYGO_EXECUTABLE tinygo)
  if (NOT TINYGO_EXECUTABLE)
    message(WARNING "TinyGo compiler 'tinygo' not found. Go example builds are disabled.")
  else ()
    set(GO_AVAILABLE ON)
    set(GO_AVAILABLE ON PARENT_SCOPE)
  endif ()
endif ()

if (GO_AVAILABLE)
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${TINYGO_EXECUTABLE}
            build
            -target=wasm
            -opt=z
            -no-debug
            -panic=trap
            -scheduler=none
            -gc=leaking
            -o ${WASM_HELLO_GO_WASM}
            ${WASM_HELLO_GO_SRC}
    DEPENDS ${WASM_HELLO_GO_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building wasm hello-go module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            ${WASM_HELLO_GO_WASM}
            ${WASM_HELLO_GO_APP}
            hello-go
            wasmos_entry
            16
            16
            4
            -
            0
            ipc.basic
            0
    DEPENDS ${WASM_HELLO_GO_WASM} make_wasmos_app
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm hello-go as WASMOS-APP"
    VERBATIM
  )
  add_custom_target(go_examples DEPENDS ${WASM_HELLO_GO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_GO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS go_examples)
endif ()
