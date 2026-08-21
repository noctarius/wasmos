# WasmosAssemblyScript.cmake
# Shared helper for compiling any AssemblyScript module (drivers, apps, utils).
#
# Why a stage directory
# =====================
# `asc` resolves imports relative to the entry file and has no include path, so
# every AS source in the repo imports its siblings as "./wasmos", "./coroutine"
# and so on. Those siblings live in different directories in the tree
# (src/libc/assemblyscript, abi/generated/assemblyscript, the module's own
# directory), so the build copies them FLAT into one stage directory and points
# asc at that. The flat "./name" import convention is a consequence of this, not
# a style choice.
#
# Every AS module is compiled through tools/as_coroutine_transform.mjs, which
# lowers @coroutine functions into their state machines. It is inert for a
# module that uses no coroutines, so it applies unconditionally rather than
# being a per-module opt-in that has to be remembered.
#
# The whole AS libc is staged for every module. asc compiles only what the entry
# transitively imports, so an unreferenced file costs nothing, and the
# alternative -- each module listing the libc files it happens to use -- means
# every new libc module has to be added to every caller.

# AS libc modules staged into every AssemblyScript build.
set(WASMOS_AS_LIBC_SOURCES
    ${CMAKE_SOURCE_DIR}/src/libc/assemblyscript/wasmos.ts
    ${CMAKE_SOURCE_DIR}/src/libc/assemblyscript/coroutine.ts
    ${CMAKE_SOURCE_DIR}/src/libc/assemblyscript/eventloop.ts
    ${CMAKE_SOURCE_DIR}/abi/generated/assemblyscript/wasmos_imports.ts
    ${CMAKE_SOURCE_DIR}/abi/generated/assemblyscript/wasmos_status.ts
    CACHE INTERNAL "AssemblyScript libc sources staged for every AS module")

# wasmos_assemblyscript_compile(
#   NAME        <name>   # build-comment label
#   ENTRY       <path>   # the module's .ts source
#   OUTPUT_WASM <path>   # output .wasm
#   MANIFEST    <path>   # decides the entry convention and the link memory
#   [EXTRA_SOURCES <...>]  # further .ts staged beside the entry
# )
#
# Compiles through the SDK's wasmos-asc driver, which owns the staging, the
# coroutine transform, --runtime stub and the [link] memory conversion. Packing the
# result into a .wap is the caller's business, since drivers and apps pack
# differently.
#
# ENTRY_NAME and INITIAL_MEMORY_PAGES are gone: the driver stages the entry under
# whichever name the manifest's entry convention requires, and reads the memory from
# [link] in bytes.
function(wasmos_assemblyscript_compile)
  cmake_parse_arguments(ARG "" "NAME;ENTRY;OUTPUT_WASM;MANIFEST;ENTRY_NAME;INITIAL_MEMORY_PAGES"
                        "EXTRA_SOURCES" ${ARGN})

  foreach (_required NAME ENTRY OUTPUT_WASM MANIFEST)
    if (NOT ARG_${_required})
      message(FATAL_ERROR "wasmos_assemblyscript_compile: missing required argument ${_required}")
    endif ()
  endforeach ()
  if (ARG_ENTRY_NAME)
    message(FATAL_ERROR
      "${ARG_NAME}: ENTRY_NAME is decided by the manifest's entry now -- an app is "
      "staged as app.ts behind runtime.ts, a driver under its own name. Remove it.")
  endif ()
  if (ARG_INITIAL_MEMORY_PAGES)
    message(FATAL_ERROR
      "${ARG_NAME}: INITIAL_MEMORY_PAGES is read from the manifest's [link] section "
      "now, as initial_memory in BYTES. Move it there.")
  endif ()

  add_custom_command(
    OUTPUT ${ARG_OUTPUT_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${WASMOS_SDK_DIR}/bin/wasmos-asc
            --emit-wasm
            --wasmos-manifest=${ARG_MANIFEST}
            ${ARG_ENTRY}
            ${ARG_EXTRA_SOURCES}
            -o ${ARG_OUTPUT_WASM}
    DEPENDS ${ARG_ENTRY} ${WASMOS_AS_LIBC_SOURCES} ${ARG_EXTRA_SOURCES} ${ARG_MANIFEST}
            ${WASMOS_SDK_STAMP}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript ${ARG_NAME} module"
    VERBATIM
  )
endfunction()
