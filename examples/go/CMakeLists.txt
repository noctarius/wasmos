if (GO_ENABLE)
  find_program(TINYGO_EXECUTABLE tinygo)
  if (NOT TINYGO_EXECUTABLE)
    message(WARNING "TinyGo compiler 'tinygo' not found. Go example builds are disabled.")
  else ()
    # TinyGo's wasm target invokes Binaryen itself. Resolve it here rather
    # than relying on the child process's PATH from BUILD_DIR.
    find_program(WASMOPT_EXECUTABLE wasm-opt
      HINTS ${CMAKE_SOURCE_DIR}/node_modules/.bin)
    if (NOT WASMOPT_EXECUTABLE)
      message(WARNING "Binaryen tool 'wasm-opt' not found. Go example builds are disabled.")
    else ()
      set(GO_AVAILABLE ON)
      set(GO_AVAILABLE ON PARENT_SCOPE)
    endif ()
  endif ()
endif ()

if (GO_AVAILABLE)
  set(WASM_HELLO_GO_STAGE_DIR ${BUILD_DIR}/go_hello_src)
  set(WASM_HELLO_GO_LIBC_SRC ${LIBC_DIR}/go/wasmos.go)
  # The Go driver owns the TinyGo target file, the staging and the flags: it asks
  # tinygo for its root and generates the target per invocation, because a target's
  # extra-files are resolved relative to TINYGOROOT. All of that used to be spelled
  # out here, against a target template that has been removed with it -- a second
  # copy of one toolchain, and the copy the tree exercised.
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-tinygo
            --emit-wasm
            --wasmos-manifest=${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
            ${WASM_HELLO_GO_SRC}
            -o ${WASM_HELLO_GO_WASM}
    DEPENDS ${WASM_HELLO_GO_SRC} ${LIBC_DIR}/go/wasmos.go ${LIBC_DIR}/go/coroutine.go
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata ${WASMOS_SDK_STAMP}
    WORKING_DIRECTORY ${BUILD_DIR}
    COMMENT "Building wasm hello-go module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
            --in ${WASM_HELLO_GO_WASM}
            --out ${WASM_HELLO_GO_APP}
    DEPENDS ${WASM_HELLO_GO_WASM} make_wasmos_app
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm hello-go as WASMOS-APP"
    VERBATIM
  )
  add_custom_target(go_examples DEPENDS ${WASM_HELLO_GO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_GO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS go_examples)
endif ()
