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
#   NAME                  <name>   # stage directory name and build comment
#   ENTRY                 <path>   # entry .ts; staged under ENTRY_NAME
#   OUTPUT_WASM           <path>   # output .wasm
#   [ENTRY_NAME           <file>]  # staged entry filename (default: ENTRY's name)
#   [MANIFEST             <path>]  # source of --initialMemory, via [link]
#   [EXTRA_SOURCES        <...>]   # further .ts staged flat beside the entry
# )
#
# Declares the custom command producing OUTPUT_WASM. Packing the result into a
# .wap is the caller's business, since drivers and apps pack differently.
function(wasmos_assemblyscript_compile)
  cmake_parse_arguments(ARG "" "NAME;ENTRY;OUTPUT_WASM;ENTRY_NAME;MANIFEST;INITIAL_MEMORY_PAGES"
                        "EXTRA_SOURCES" ${ARGN})

  foreach (_required NAME ENTRY OUTPUT_WASM)
    if (NOT ARG_${_required})
      message(FATAL_ERROR "wasmos_assemblyscript_compile: missing required argument ${_required}")
    endif ()
  endforeach ()

  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  if (NOT ARG_ENTRY_NAME)
    get_filename_component(ARG_ENTRY_NAME "${ARG_ENTRY}" NAME)
  endif ()

  set(_stage_dir ${BUILD_DIR}/assemblyscript_${ARG_NAME}_src)
  set(_stage_cmds "")
  foreach (_src ${WASMOS_AS_LIBC_SOURCES} ${ARG_EXTRA_SOURCES})
    get_filename_component(_bname "${_src}" NAME)
    list(APPEND _stage_cmds
         COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_stage_dir}/${_bname}")
  endforeach ()

  # --initialMemory is asc's declared initial memory in WASM pages. It comes from
  # the manifest's [link] initial_memory (in bytes, like every other language), so
  # an app is sized in one file whatever it is written in. INITIAL_MEMORY_PAGES is
  # rejected rather than honoured: two sources for one number is how it goes stale.
  if (ARG_INITIAL_MEMORY_PAGES)
    message(FATAL_ERROR
      "${ARG_NAME}: INITIAL_MEMORY_PAGES is read from the manifest's [link] section "
      "now, as initial_memory in BYTES. Move it there.")
  endif ()
  set(_initial_memory_args "")
  if (ARG_MANIFEST)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND
                 PROPERTY CMAKE_CONFIGURE_DEPENDS ${ARG_MANIFEST})
    wasmos_manifest_link_pages(_as_initial_pages "${ARG_MANIFEST}" initial_memory 0)
    if (_as_initial_pages GREATER 0)
      set(_initial_memory_args --initialMemory ${_as_initial_pages})
    endif ()
  endif ()

  add_custom_command(
    OUTPUT ${ARG_OUTPUT_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARG_ENTRY} ${_stage_dir}/${ARG_ENTRY_NAME}
    ${_stage_cmds}
    COMMAND ${ASC_EXECUTABLE}
            ${_stage_dir}/${ARG_ENTRY_NAME}
            --transform ${CMAKE_SOURCE_DIR}/tools/as_coroutine_transform.mjs
            --target release
            -Osize
            --runtime stub
            --noAssert
            ${_initial_memory_args}
            --outFile ${ARG_OUTPUT_WASM}
    DEPENDS ${ARG_ENTRY} ${WASMOS_AS_LIBC_SOURCES} ${ARG_EXTRA_SOURCES}
            ${CMAKE_SOURCE_DIR}/tools/as_coroutine_transform.mjs
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript ${ARG_NAME} module"
    VERBATIM
  )
endfunction()
