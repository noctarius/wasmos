set(WASM_HELLO_RUST_LIBC_SRC ${LIBC_DIR}/rust/wasmos.rs)
set(WASM_HELLO_RUST_COROUTINE_SRC ${LIBC_DIR}/rust/coroutine.rs)
set(WASM_HELLO_RUST_COROUTINE_OBJ ${BUILD_DIR}/wasmos_rust_coroutine.o)
set(WASM_HELLO_RUST_IPC_FUTURE_OBJ ${BUILD_DIR}/wasmos_rust_ipc_future.o)
set(WASM_HELLO_RUST_SERVICE_RUNTIME_OBJ ${BUILD_DIR}/wasmos_rust_service_runtime.o)

if (RUST_ENABLE)
  find_program(RUSTC_EXECUTABLE rustc)
  if (NOT RUSTC_EXECUTABLE)
    message(FATAL_ERROR "Rust compiler 'rustc' not found. Install Rust and add the wasm32-unknown-unknown target (rustup target add wasm32-unknown-unknown).")
  endif ()
  # Compiles through the SDK's wasmos-rustc driver, which owns the staging (the
  # binding is a sibling module), the small shadow stack, the wasm link flags and
  # libsys.a -- where the coroutine, event-loop and async-filesystem entry points
  # the binding declares as extern "C" live. Building those three shims as objects
  # here and spelling out the link line was the in-tree copy of all of that.
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-rustc
            --emit-wasm
            --wasmos-manifest=${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
            ${WASM_HELLO_RUST_SRC}
            -o ${WASM_HELLO_RUST_WASM}
    DEPENDS ${WASM_HELLO_RUST_SRC} ${LIBC_DIR}/rust/wasmos.rs
            ${LIBC_DIR}/rust/coroutine.rs
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata ${WASMOS_SDK_STAMP}
    WORKING_DIRECTORY ${BUILD_DIR}
    COMMENT "Building wasm hello-rust module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
            --in ${WASM_HELLO_RUST_WASM}
            --out ${WASM_HELLO_RUST_APP}
    DEPENDS ${WASM_HELLO_RUST_WASM} make_wasmos_app
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm hello-rust as WASMOS-APP"
    VERBATIM
  )
  # --- Tetris: two-player networked graphical Rust app ---------------------
  add_custom_command(
    OUTPUT ${WASM_TETRIS_RUST_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-rustc
            --emit-wasm
            --wasmos-manifest=${CMAKE_CURRENT_SOURCE_DIR}/tetris/linker.metadata
            ${WASM_TETRIS_RUST_SRC}
            ${CMAKE_CURRENT_SOURCE_DIR}/tetris/net_shim.c
            -o ${WASM_TETRIS_RUST_WASM}
    DEPENDS ${WASM_TETRIS_RUST_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/tetris/net_shim.c
            ${LIBC_DIR}/rust/wasmos.rs ${LIBC_DIR}/rust/coroutine.rs
            ${CMAKE_CURRENT_SOURCE_DIR}/tetris/linker.metadata ${WASMOS_SDK_STAMP}
    WORKING_DIRECTORY ${BUILD_DIR}
    COMMENT "Building wasm tetris module"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_TETRIS_RUST_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/tetris/linker.metadata
            --in ${WASM_TETRIS_RUST_WASM}
            --out ${WASM_TETRIS_RUST_APP}
    DEPENDS ${WASM_TETRIS_RUST_WASM} make_wasmos_app
            ${CMAKE_CURRENT_SOURCE_DIR}/tetris/linker.metadata
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing wasm tetris as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(rust_examples DEPENDS ${WASM_HELLO_RUST_APP} ${WASM_TETRIS_RUST_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_RUST_APP} ${WASM_TETRIS_RUST_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS rust_examples)
endif ()
