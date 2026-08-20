# WasmosZigApp.cmake
# Shared helper for building any Zig WASM app (utilities, examples, services).
#
# Why --stack 8192 is mandatory
# ==============================
# Each process context gets one MEM_REGION_WASM_LINEAR user-VA mirror region of
# 16 × 4 KiB pages (64 KiB), matching a module's one-page initial linear memory
# (src/kernel/memory.c, mm_context_alloc_region(ctx, 16, ...)).  Every hostcall
# that writes to WASM memory — proc_info_stats, fs_buffer_write, fs_buffer_copy,
# etc. — calls mm_user_range_permitted, which walks only that region: a pointer
# whose offset lands above it is rejected and the call fails silently.
# Zig's default shadow stack is 1 MB, which places globals at ~1 MB, past the
# region entirely.  --stack 8192 mirrors the layout of C WASM modules
# (stack_ptr = 0x2000) and keeps globals in the region's first pages.
#
# The wasm_stack_check.py script verifies this after every compilation and
# fails the build immediately if the constraint is violated.

# Budget wasm_stack_check.py enforces on stack pointer + data end.  32 KiB is
# half the kernel's 64 KiB MEM_REGION_WASM_LINEAR window, so a passing module is
# inside it with room to spare.
# TODO: no Zig app has needed more than 32 KiB of data, so nobody has decided
# whether this should track the kernel window (65536) instead; a Zig app that
# outgrows the budget will fail the check while the kernel would still accept
# it, and the number must be re-derived then rather than nudged.
set(WASMOS_ZIG_USER_VA_LIMIT 32768 CACHE INTERNAL
    "Layout budget (bytes) validated by wasm_stack_check.py; see note above")

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
  cmake_parse_arguments(ARG "" "NAME;TARGET;SRC;LIBC_SRC;OUTPUT_WASM;OUTPUT_APP;MANIFEST;INITIAL_MEMORY"
                        "EXTRA_SRCS;INCLUDE_DIRS" ${ARGN})

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
  get_filename_component(_libc_src_dir "${ARG_LIBC_SRC}" DIRECTORY)
  set(_coroutine_zig "${_libc_src_dir}/coroutine.zig")

  # Build -I flag list from INCLUDE_DIRS
  set(_include_flags "")
  foreach(_dir ${ARG_INCLUDE_DIRS})
    list(APPEND _include_flags "-I${_dir}")
  endforeach()

  # Collect stage commands and staged paths for EXTRA_SRCS.
  # .zig extras are staged to _stage/ so @import("name.zig") resolves them.
  # .c extras are passed by their original absolute path (no staging needed).
  set(_stage_cmds "")
  set(_extra_staged "")    # .zig files staged into _stage
  set(_extra_c "")         # .c files passed directly
  foreach(_src ${ARG_EXTRA_SRCS})
    get_filename_component(_bname "${_src}" NAME)
    if("${_bname}" MATCHES "\\.zig$")
      list(APPEND _stage_cmds
           COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_stage}/${_bname}")
      list(APPEND _extra_staged "${_stage}/${_bname}")
    else()
      list(APPEND _extra_c "${_src}")
    endif()
  endforeach()

  # -Xlinker --initial-memory=N sets the WASM binary's declared initial memory
  # pages.  WARP's ActiveMemoryManager derives allowedLinMemPages_ from it;
  # without it a Zig freestanding binary declares one page (64 KiB) and a shmem
  # probe beyond that throws OutOfBounds at runtime -- which reaches C++ code with
  # no handler and panics the kernel, so this is not a tuning knob.
  #
  # The value comes from the manifest's [link] section, the same place the C helper
  # reads it, so an app is sized in one file whatever language it is written in.
  # ARG_INITIAL_MEMORY is rejected rather than honoured: two sources for one number
  # is how it goes stale.
  if (ARG_INITIAL_MEMORY)
    message(FATAL_ERROR
      "${ARG_NAME}: INITIAL_MEMORY is read from the manifest's [link] section now. "
      "Move it to ${ARG_MANIFEST} as initial_memory.")
  endif ()
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND
               PROPERTY CMAKE_CONFIGURE_DEPENDS ${ARG_MANIFEST})
  wasmos_manifest_link_value(_zig_initial_memory "${ARG_MANIFEST}" initial_memory 0)
  set(_initial_mem_flags "")
  if (_zig_initial_memory GREATER 0)
    set(_initial_mem_flags --initial-memory=${_zig_initial_memory})
  endif ()

  set(_c_compile_flags "")
  if (_extra_c)
    set(_c_compile_flags -cflags)
    foreach(_dir ${ARG_INCLUDE_DIRS})
      list(APPEND _c_compile_flags "-I${_dir}")
    endforeach()
    list(APPEND _c_compile_flags --)
  endif ()

  add_custom_command(
    OUTPUT ${ARG_OUTPUT_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_cache}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gcache}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARG_SRC}      ${_stage}/${ARG_NAME}.zig
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARG_LIBC_SRC} ${_stage}/wasmos.zig
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_coroutine_zig} ${_stage}/coroutine.zig
    ${_stage_cmds}
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
            ${_initial_mem_flags}
            ${_include_flags}
            ${_stage}/${ARG_NAME}.zig
            ${_c_compile_flags}
            ${_extra_c}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/wasm_stack_check.py
            ${ARG_OUTPUT_WASM}
            --stack-size ${WASMOS_ZIG_STACK_SIZE}
            --max-addr   ${WASMOS_ZIG_USER_VA_LIMIT}
    DEPENDS ${ARG_SRC} ${ARG_LIBC_SRC} ${_coroutine_zig} ${ARG_EXTRA_SRCS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building and validating Zig WASM app: ${ARG_NAME}"
    VERBATIM
  )

  wasmos_maybe_aot_pack(
    "${ARG_MANIFEST}"
    "${ARG_OUTPUT_WASM}"
    "${ARG_OUTPUT_APP}"
  )

  add_custom_target(${ARG_TARGET} DEPENDS ${ARG_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS        ${ARG_OUTPUT_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS ${ARG_TARGET})
endfunction()
