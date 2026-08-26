# WasmosZigApp.cmake
# Shared helper for building any Zig WASM app (utilities, examples, services).
#
# Why --stack 8192
# ================
# Zig's default shadow stack is 1 MB and is placed FIRST, so a module's globals
# land above 1 MB and its declared linear memory must be at least 2 MiB.  8192
# gives the layout a C module has (stack_ptr = 0x2000, globals just above it),
# which is what lets a Zig app declare a single 64 KiB page like every C app.
# That is the reason the flag is passed: module size, not correctness.
#
# It is NOT a pointer-validity constraint, though it was documented as one here
# for a long time.  The claim was that MEM_REGION_WASM_LINEAR is a fixed 16-page
# (64 KiB) user-VA mirror, so a host call handed a pointer above it fails
# silently.  That stopped being true when reserved-VA linmem landed:
# mm_context_rebind_wasm_linear and mm_context_bind_wasm_linear_scattered
# (src/kernel/memory.c) REPOINT AND RESIZE that region to the guest's actual
# linear memory, so the 16 pages allocated at context creation are a bootstrap
# default, not the bound a running guest is checked against.
#
# Measured rather than reasoned, on 2026-08-21: a Zig module built with
# --stack 1048576 (data at 0x100000, failing the check below outright) executes
# console_write AND reads its spawn-info buffer through xfer_buffer_read, with
# pointers above 1 MB, under BOTH runtimes -- WARP and wasm3.  In tree,
# examples/rust/hello has had data at 0x100000 all along for the same reason and
# passes its guest test.  See docs/TASKS.md before treating the budget below as a
# safety property.

# Budget wasm_stack_check.py enforces on stack pointer + data end.  With the
# constraint above corrected, this is a check that the small stack was actually
# applied -- a module that quietly reverted to Zig's 1 MB default trips it -- not
# a bound the kernel imposes.  Kept because a module accidentally declaring 2 MiB
# of linear memory is worth catching at build time.
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

  if (NOT ARG_NAME OR NOT ARG_TARGET OR NOT ARG_SRC OR
      NOT ARG_OUTPUT_WASM OR NOT ARG_OUTPUT_APP OR NOT ARG_MANIFEST)
    message(FATAL_ERROR "wasmos_add_zig_wasm_app: missing required argument")
  endif ()
  if (ARG_LIBC_SRC)
    message(FATAL_ERROR
      "${ARG_NAME}: LIBC_SRC is staged by the driver now (share/wasmos/zig). Remove it.")
  endif ()
  if (ARG_INCLUDE_DIRS)
    message(FATAL_ERROR
      "${ARG_NAME}: INCLUDE_DIRS is the sysroot now. Remove it.")
  endif ()
  # coroutine.zig is one of the shims the driver stages, so an EXTRA_SRCS entry for
  # it would be staged twice; drop it rather than making every caller remember.
  set(_zig_extra_srcs ${ARG_EXTRA_SRCS})
  list(REMOVE_ITEM _zig_extra_srcs ${LIBC_DIR}/zig/coroutine.zig)

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
  # Read only to register the configure-time dependency above; the [link] values
  # themselves are read by the wasmos-zig driver, which is the single place they
  # are turned into linker flags.
  wasmos_manifest_link_value(_zig_initial_memory "${ARG_MANIFEST}" initial_memory 0)

  set(_c_compile_flags "")
  if (_extra_c)
    set(_c_compile_flags -cflags)
    foreach(_dir ${ARG_INCLUDE_DIRS})
      list(APPEND _c_compile_flags "-I${_dir}")
    endforeach()
    list(APPEND _c_compile_flags --)
  endif ()

  # Compiles through the SDK's wasmos-zig driver, which owns the staging (Zig
  # resolves @import beside the importing file), the mandatory small shadow stack,
  # the layout check and the [link] memory. LIBC_SRC and INCLUDE_DIRS are gone with
  # it: the driver stages its own runtime shims and compiles extra C against the
  # sysroot.
  add_custom_command(
    OUTPUT ${ARG_OUTPUT_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-zig
            --emit-wasm
            --wasmos-manifest=${ARG_MANIFEST}
            ${ARG_SRC}
            ${_zig_extra_srcs}
            -o ${ARG_OUTPUT_WASM}
    # The SDK stamp covers the wasmos-zig driver and the staged runtime shims:
    # without it, a change to how a module is COMPILED (an added linker flag, a
    # shim edit) leaves every module built from the old rules and still marked
    # up to date.
    DEPENDS ${ARG_SRC} ${ARG_EXTRA_SRCS} ${ARG_MANIFEST} ${WASMOS_SDK_STAMP}
            ${LIBC_DIR}/zig/wasmos.zig ${LIBC_DIR}/zig/coroutine.zig
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
