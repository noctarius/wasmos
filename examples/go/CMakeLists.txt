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
  set(WASM_HELLO_GO_STAGE_DIR ${BUILD_DIR}/go_hello_src)
  set(WASM_HELLO_GO_LIBC_SRC ${LIBC_DIR}/go/wasmos.go)
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_HELLO_GO_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_HELLO_GO_SRC} ${WASM_HELLO_GO_STAGE_DIR}/hello_go.go
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_HELLO_GO_LIBC_SRC} ${WASM_HELLO_GO_STAGE_DIR}/wasmos.go
    COMMAND ${CMAKE_COMMAND} -E env GO111MODULE=off
            ${TINYGO_EXECUTABLE}
            build
            -target=wasm
            -opt=z
            -no-debug
            -panic=trap
            -scheduler=none
            -gc=leaking
            -o ${WASM_HELLO_GO_WASM}
            ./go_hello_src
    DEPENDS ${WASM_HELLO_GO_SRC} ${WASM_HELLO_GO_LIBC_SRC}
    WORKING_DIRECTORY ${BUILD_DIR}
    COMMENT "Building wasm hello-go module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.manifest
            --in ${WASM_HELLO_GO_WASM}
            --out ${WASM_HELLO_GO_APP}
    DEPENDS ${WASM_HELLO_GO_WASM} make_wasmos_app
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.manifest
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm hello-go as WASMOS-APP"
    VERBATIM
  )
  add_custom_target(go_examples DEPENDS ${WASM_HELLO_GO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_GO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS go_examples)
endif ()
