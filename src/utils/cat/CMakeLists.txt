if (ZIG_ENABLE)
  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (ZIG_EXECUTABLE)
    set(WASM_CAT_UTIL_STAGE_DIR ${BUILD_DIR}/zig_cat_src)
    set(WASM_CAT_UTIL_CACHE_DIR ${BUILD_DIR}/zig_cache)
    set(WASM_CAT_UTIL_GLOBAL_CACHE_DIR ${BUILD_DIR}/zig_global_cache)
    set(WASM_CAT_LIBC_SRC ${LIBC_DIR}/zig/wasmos.zig)

    add_custom_command(
      OUTPUT ${WASM_CAT_WASM}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_CAT_UTIL_STAGE_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_CAT_UTIL_CACHE_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_CAT_UTIL_GLOBAL_CACHE_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_CAT_SRC} ${WASM_CAT_UTIL_STAGE_DIR}/cat.zig
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_CAT_LIBC_SRC} ${WASM_CAT_UTIL_STAGE_DIR}/wasmos.zig
      COMMAND ${ZIG_EXECUTABLE}
              build-exe
              -target wasm32-freestanding
              -O ReleaseSmall
              -fno-entry
              -fstrip
              --export=wasmos_main
              --stack 8192
              --cache-dir ${WASM_CAT_UTIL_CACHE_DIR}
              --global-cache-dir ${WASM_CAT_UTIL_GLOBAL_CACHE_DIR}
              -femit-bin=${WASM_CAT_WASM}
              ${WASM_CAT_UTIL_STAGE_DIR}/cat.zig
      DEPENDS ${WASM_CAT_SRC} ${WASM_CAT_LIBC_SRC}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Building Zig cat utility"
      VERBATIM
    )

    add_custom_command(
      OUTPUT ${WASM_CAT_APP}
      COMMAND ${WASMOS_APP_PACKER}
              --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
              --in ${WASM_CAT_WASM}
              --out ${WASM_CAT_APP}
      DEPENDS ${WASM_CAT_WASM} make_wasmos_app
              ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Packing cat utility as WASMOS-APP"
      VERBATIM
    )

    add_custom_target(cat_util DEPENDS ${WASM_CAT_APP})
    set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_CAT_APP})
    set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS cat_util)
  endif ()
endif ()
