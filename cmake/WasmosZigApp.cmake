# WasmosZigApp.cmake
# Shared helper for building any Zig WASM app (utilities, examples, services).
#
# Why --stack 8192 is mandatory
# ==============================
# The kernel's mm_context_alloc_region allocates 8 × 4 KB pages (32 KB) for
# MEM_REGION_WASM_LINEAR per process context.  Every hostcall that writes to
# WASM memory — proc_info_stats, fs_buffer_write, fs_buffer_copy, etc. —
# calls mm_user_range_permitted, which walks only that 32 KB window.
# Zig's default shadow stack is 1 MB, which places globals at ~1 MB: every
# such hostcall rejects the pointer and fails silently.  Building with
# --stack 8192 mirrors the layout of C WASM modules (stack_ptr = 0x2000) and
# keeps all globals well within 32 KB.
#
# The wasm_stack_check.py script verifies this after every compilation and
# fails the build immediately if the constraint is violated.

# Kernel user-VA region limit: 8 pages × 4 KB.  Must stay in sync with
# mm_context_alloc_region(ctx, 8, ..., MEM_REGION_WASM_LINEAR) in
# src/kernel/memory.c.
set(WASMOS_ZIG_USER_VA_LIMIT 32768 CACHE INTERNAL
    "Kernel user-VA region size (bytes) validated by wasm_stack_check.py")

# Shadow-stack size passed to zig build-exe for every Zig WASM app.
set(WASMOS_ZIG_STACK_SIZE 8192 CACHE INTERNAL
    "Zig WASM shadow-stack size (--stack N); must keep data below WASMOS_ZIG_USER_VA_LIMIT")

# wasmos_add_zig_wasm_app(
#   NAME         <name>          # app name — used for stage dir and comments
#   TARGET       <cmake_target>  # CMake target name (e.g. zig_examples, ps_util)
#   SRC          <path>          # source .zig file
#   LIBC_SRC     <path>          # wasmos.zig wrapper
#   OUTPUT_WASM  <path>          # output .wasm path
#   OUTPUT_APP   <path>          # output .wap path
#   MANIFEST     <path>          # linker.metadata
# )
#
# Creates a CMake target named TARGET that:
#   1. stages sources → zig build-exe --stack WASMOS_ZIG_STACK_SIZE
#   2. runs wasm_stack_check.py (build fails on layout violation)
#   3. packs the result with the app packer
#
# Appends OUTPUT_APP to WASMOS_WASM_APPS and TARGET to
# WASMOS_WASM_APP_TARGETS so QEMU targets pick up the dependency.
# Does nothing when ZIG_ENABLE is OFF or zig is not found.
function(wasmos_add_zig_wasm_app)
  cmake_parse_arguments(ARG "" "NAME;TARGET;SRC;LIBC_SRC;OUTPUT_WASM;OUTPUT_APP;MANIFEST" "" ${ARGN})

  if (NOT ARG_NAME OR NOT ARG_TARGET OR NOT ARG_SRC OR NOT ARG_LIBC_SRC OR
      NOT ARG_OUTPUT_WASM OR NOT ARG_OUTPUT_APP OR NOT ARG_MANIFEST)
    message(FATAL_ERROR "wasmos_add_zig_wasm_app: missing required argument")
  endif ()

  if (NOT ZIG_ENABLE)
    return()
  endif ()

  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (NOT ZIG_EXECUTABLE)
    message(WARNING "zig not found; ${ARG_NAME} Zig WASM app will be skipped")
    return()
  endif ()

  set(_stage  "${BUILD_DIR}/zig_${ARG_NAME}_src")
  set(_cache  "${BUILD_DIR}/zig_cache")
  set(_gcache "${BUILD_DIR}/zig_global_cache")

  add_custom_command(
    OUTPUT ${ARG_OUTPUT_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_cache}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gcache}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARG_SRC}      ${_stage}/${ARG_NAME}.zig
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARG_LIBC_SRC} ${_stage}/wasmos.zig
    COMMAND ${ZIG_EXECUTABLE}
            build-exe
            -target wasm32-freestanding
            -O ReleaseSmall
            -fno-entry
            -fstrip
            --export=wasmos_main
            --stack ${WASMOS_ZIG_STACK_SIZE}
            --cache-dir        ${_cache}
            --global-cache-dir ${_gcache}
            -femit-bin=${ARG_OUTPUT_WASM}
            ${_stage}/${ARG_NAME}.zig
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/wasm_stack_check.py
            ${ARG_OUTPUT_WASM}
            --stack-size ${WASMOS_ZIG_STACK_SIZE}
            --max-addr   ${WASMOS_ZIG_USER_VA_LIMIT}
    DEPENDS ${ARG_SRC} ${ARG_LIBC_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building and validating Zig WASM app: ${ARG_NAME}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ARG_OUTPUT_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${ARG_MANIFEST}
            --in  ${ARG_OUTPUT_WASM}
            --out ${ARG_OUTPUT_APP}
    DEPENDS ${ARG_OUTPUT_WASM} make_wasmos_app ${ARG_MANIFEST}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing Zig WASM app: ${ARG_NAME}"
    VERBATIM
  )

  add_custom_target(${ARG_TARGET} DEPENDS ${ARG_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS        ${ARG_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ${ARG_TARGET})
endfunction()
