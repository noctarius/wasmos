set(WASM_HELLO_RUST_LIBC_SRC ${LIBC_DIR}/rust/wasmos.rs)

if (RUST_ENABLE)
  find_program(RUSTC_EXECUTABLE rustc)
  if (NOT RUSTC_EXECUTABLE)
    message(FATAL_ERROR "Rust compiler 'rustc' not found. Install Rust and add the wasm32-unknown-unknown target (rustup target add wasm32-unknown-unknown).")
  endif ()
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${RUSTC_EXECUTABLE}
            --target=wasm32-unknown-unknown
            -O
            -C opt-level=z
            -C panic=abort
            -C codegen-units=1
            -C link-arg=--no-entry
            -C link-arg=--export=wasmos_main
            -C link-arg=--strip-all
            -C link-arg=--initial-memory=2097152
            -C link-arg=--max-memory=2097152
            --crate-type=cdylib
            ${WASM_HELLO_RUST_SRC}
            -o ${WASM_HELLO_RUST_WASM}
    DEPENDS ${WASM_HELLO_RUST_SRC} ${WASM_HELLO_RUST_LIBC_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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
  add_custom_target(rust_examples DEPENDS ${WASM_HELLO_RUST_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_RUST_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS rust_examples)
endif ()
