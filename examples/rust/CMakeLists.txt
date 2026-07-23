set(WASM_HELLO_RUST_LIBC_SRC ${LIBC_DIR}/rust/wasmos.rs)
set(WASM_HELLO_RUST_COROUTINE_SRC ${LIBC_DIR}/rust/coroutine.rs)
set(WASM_HELLO_RUST_COROUTINE_OBJ ${BUILD_DIR}/wasmos_rust_coroutine.o)
set(WASM_HELLO_RUST_IPC_FUTURE_OBJ ${BUILD_DIR}/wasmos_rust_ipc_future.o)

if (RUST_ENABLE)
  find_program(RUSTC_EXECUTABLE rustc)
  if (NOT RUSTC_EXECUTABLE)
    message(FATAL_ERROR "Rust compiler 'rustc' not found. Install Rust and add the wasm32-unknown-unknown target (rustup target add wasm32-unknown-unknown).")
  endif ()
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_COROUTINE_OBJ}
    COMMAND ${CLANG} --target=wasm32 -Oz -ffreestanding
            -I${LIBSYS_WASM_DIR}/include
            -c ${LIBSYS_WASM_DIR}/coroutine_wasm.c
            -o ${WASM_HELLO_RUST_COROUTINE_OBJ}
    DEPENDS ${LIBSYS_WASM_DIR}/coroutine_wasm.c
            ${LIBSYS_WASM_DIR}/include/wasmos/coroutine_wasm.h
    COMMENT "Building WASM coroutine core for Rust"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${WASM_HELLO_RUST_IPC_FUTURE_OBJ}
    COMMAND ${CLANG} --target=wasm32 -Oz -ffreestanding
            -I${LIBSYS_WASM_DIR}/include -I${LIBC_DIR}/include -I${DRIVER_WASM_DIR}/include
            -c ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
            -o ${WASM_HELLO_RUST_IPC_FUTURE_OBJ}
    DEPENDS ${LIBSYS_WASM_DIR}/ipc_future_wasm.c ${LIBSYS_WASM_DIR}/include/wasmos/libsys.h
    COMMENT "Building WASM IPC future adapter for Rust"
    VERBATIM
  )
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
            -C link-arg=${WASM_HELLO_RUST_COROUTINE_OBJ}
            -C link-arg=${WASM_HELLO_RUST_IPC_FUTURE_OBJ}
            --crate-type=cdylib
            ${WASM_HELLO_RUST_SRC}
            -o ${WASM_HELLO_RUST_WASM}
    DEPENDS ${WASM_HELLO_RUST_SRC} ${WASM_HELLO_RUST_LIBC_SRC}
            ${WASM_HELLO_RUST_COROUTINE_SRC} ${WASM_HELLO_RUST_COROUTINE_OBJ}
            ${WASM_HELLO_RUST_IPC_FUTURE_OBJ}
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
