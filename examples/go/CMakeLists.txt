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
  execute_process(
    COMMAND ${TINYGO_EXECUTABLE} env TINYGOROOT
    OUTPUT_VARIABLE TINYGO_ROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if (NOT TINYGO_ROOT)
    message(FATAL_ERROR "TinyGo did not report TINYGOROOT")
  endif ()
  file(RELATIVE_PATH TINYGO_COROUTINE_C
    "${TINYGO_ROOT}" "${LIBSYS_WASM_DIR}/coroutine_wasm.c")
  file(RELATIVE_PATH TINYGO_IPC_FUTURE_C
    "${TINYGO_ROOT}" "${LIBSYS_WASM_DIR}/ipc_future_wasm.c")
  configure_file(${LIBC_DIR}/go/wasmos-tinygo.json.in ${BUILD_DIR}/wasmos-tinygo.json @ONLY)
  set(WASM_HELLO_GO_STAGE_DIR ${BUILD_DIR}/go_hello_src)
  set(WASM_HELLO_GO_LIBC_SRC ${LIBC_DIR}/go/wasmos.go)
  add_custom_command(
    OUTPUT ${WASM_HELLO_GO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_HELLO_GO_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_HELLO_GO_SRC} ${WASM_HELLO_GO_STAGE_DIR}/hello_go.go
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_HELLO_GO_LIBC_SRC} ${WASM_HELLO_GO_STAGE_DIR}/wasmos.go
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/go/coroutine.go ${WASM_HELLO_GO_STAGE_DIR}/coroutine.go
    COMMAND ${CMAKE_COMMAND} -E env GO111MODULE=off WASMOPT=${WASMOPT_EXECUTABLE}
            ${TINYGO_EXECUTABLE}
            build
            -target=${BUILD_DIR}/wasmos-tinygo.json
            -opt=z
            -no-debug
            -panic=trap
            -scheduler=none
            -gc=leaking
            -o ${WASM_HELLO_GO_WASM}
            ./go_hello_src
    DEPENDS ${WASM_HELLO_GO_SRC} ${WASM_HELLO_GO_LIBC_SRC} ${LIBC_DIR}/go/coroutine.go
            ${LIBC_DIR}/go/wasmos-tinygo.json.in ${LIBSYS_WASM_DIR}/coroutine_wasm.c
            ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
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
