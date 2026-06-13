# WasmosZigUtil.cmake
# Helper for building Zig WASM utilities that ship in /boot/system/utils.
#
# All Zig utils must be built with --stack 8192 so their shadow stack starts
# at 0x2000, placing every global well within the kernel's 32 KB user-VA
# region (mm_context_alloc_region allocates 8 × 4 KB pages for
# MEM_REGION_WASM_LINEAR).  After compilation the wasm_stack_check.py script
# verifies the layout and fails the build early if the constraint is violated.

# Kernel user-VA region limit: 8 pages × 4096 bytes.  Must stay in sync with
# the mm_context_alloc_region(ctx, 8, ..., MEM_REGION_WASM_LINEAR) call in
# src/kernel/memory.c.
set(WASMOS_ZIG_USER_VA_LIMIT 32768 CACHE INTERNAL
    "Kernel user-VA region size (bytes) checked by wasm_stack_check.py")

# Required shadow-stack size for all Zig WASM utilities.
set(WASMOS_ZIG_STACK_SIZE 8192 CACHE INTERNAL
    "Zig WASM shadow-stack size (--stack N); must keep data below WASMOS_ZIG_USER_VA_LIMIT")

# wasmos_add_zig_util(
#   NAME       <name>          # utility name, used for target and stage dir
#   SRC        <path>          # source .zig file
#   LIBC_SRC   <path>          # wasmos.zig wrapper
#   OUTPUT_WASM <path>         # destination .wasm
#   OUTPUT_APP  <path>         # destination .wap
#   MANIFEST    <path>         # linker.metadata
# )
#
# Creates:
#   <name>_util   custom target that builds and validates the .wap
#
# Also appends OUTPUT_APP to the WASMOS_WASM_APPS global property and
# <name>_util to WASMOS_WASM_APP_TARGETS so QEMU targets pick it up.
function(wasmos_add_zig_util)
  cmake_parse_arguments(ARG "" "NAME;SRC;LIBC_SRC;OUTPUT_WASM;OUTPUT_APP;MANIFEST" "" ${ARGN})

  if (NOT ARG_NAME OR NOT ARG_SRC OR NOT ARG_LIBC_SRC OR
      NOT ARG_OUTPUT_WASM OR NOT ARG_OUTPUT_APP OR NOT ARG_MANIFEST)
    message(FATAL_ERROR "wasmos_add_zig_util: missing required argument")
  endif ()

  if (NOT ZIG_ENABLE)
    return()
  endif ()

  # Locate zig if not already cached.
  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (NOT ZIG_EXECUTABLE)
    message(WARNING "zig not found; ${ARG_NAME} utility will be skipped")
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
            --cache-dir  ${_cache}
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
    COMMENT "Building and validating Zig ${ARG_NAME} utility"
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
    COMMENT "Packing ${ARG_NAME} utility as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(${ARG_NAME}_util DEPENDS ${ARG_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS        ${ARG_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ${ARG_NAME}_util)
endfunction()
