if (ZIG_ENABLE)
  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (NOT ZIG_EXECUTABLE)
    message(WARNING "zig not found; Zig examples will be skipped.")
  else ()
    set(ZIG_AVAILABLE ON)
    set(ZIG_AVAILABLE ON PARENT_SCOPE)
  endif ()
endif ()

set(WASM_HELLO_ZIG_LIBC_SRC ${LIBC_DIR}/zig/wasmos.zig)

if (ZIG_AVAILABLE)
  set(WASM_HELLO_ZIG_STAGE_DIR ${BUILD_DIR}/zig_hello_src)
  set(WASM_HELLO_ZIG_CACHE_DIR ${BUILD_DIR}/zig_cache)
  set(WASM_HELLO_ZIG_GLOBAL_CACHE_DIR ${BUILD_DIR}/zig_global_cache)
  add_custom_command(
    OUTPUT ${WASM_HELLO_ZIG_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_HELLO_ZIG_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_HELLO_ZIG_CACHE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_HELLO_ZIG_GLOBAL_CACHE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_HELLO_ZIG_SRC} ${WASM_HELLO_ZIG_STAGE_DIR}/hello_zig.zig
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_HELLO_ZIG_LIBC_SRC} ${WASM_HELLO_ZIG_STAGE_DIR}/wasmos.zig
    COMMAND ${ZIG_EXECUTABLE}
            build-exe
            -target wasm32-freestanding
            -O ReleaseSmall
            -fno-entry
            -fstrip
            --export=wasmos_main
            --cache-dir ${WASM_HELLO_ZIG_CACHE_DIR}
            --global-cache-dir ${WASM_HELLO_ZIG_GLOBAL_CACHE_DIR}
            -femit-bin=${WASM_HELLO_ZIG_WASM}
            ${WASM_HELLO_ZIG_STAGE_DIR}/hello_zig.zig
    DEPENDS ${WASM_HELLO_ZIG_SRC} ${WASM_HELLO_ZIG_LIBC_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building Zig hello module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_HELLO_ZIG_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.manifest
            --in ${WASM_HELLO_ZIG_WASM}
            --out ${WASM_HELLO_ZIG_APP}
    DEPENDS ${WASM_HELLO_ZIG_WASM} make_wasmos_app
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.manifest
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing Zig hello as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(zig_examples DEPENDS ${WASM_HELLO_ZIG_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_HELLO_ZIG_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS zig_examples)
endif ()
