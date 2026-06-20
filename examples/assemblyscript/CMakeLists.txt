if (AS_ENABLE)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  function(wasmos_add_assemblyscript_app)
    cmake_parse_arguments(ARG "" "NAME;SRC;OUTPUT_WASM;OUTPUT_APP;MANIFEST;INITIAL_MEMORY_PAGES"
                          "EXTRA_STAGE_FILES" ${ARGN})

    if (NOT ARG_NAME OR NOT ARG_SRC OR NOT ARG_OUTPUT_WASM OR NOT ARG_OUTPUT_APP OR NOT ARG_MANIFEST)
      message(FATAL_ERROR "wasmos_add_assemblyscript_app: missing required argument")
    endif ()

    set(_stage_dir ${BUILD_DIR}/assemblyscript_${ARG_NAME}_src)
    set(_entry_src ${_stage_dir}/entry.ts)
    set(_stage_cmds "")
    foreach(_src ${ARG_EXTRA_STAGE_FILES})
      get_filename_component(_bname "${_src}" NAME)
      list(APPEND _stage_cmds
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_stage_dir}/${_bname}")
    endforeach()

    set(_initial_memory_args "")
    if (ARG_INITIAL_MEMORY_PAGES)
      set(_initial_memory_args --initialMemory ${ARG_INITIAL_MEMORY_PAGES})
    endif ()

    add_custom_command(
      OUTPUT ${ARG_OUTPUT_WASM}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage_dir}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARG_SRC} ${_stage_dir}/app.ts
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/assemblyscript/wasmos.ts ${_stage_dir}/wasmos.ts
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/assemblyscript/runtime.ts ${_entry_src}
      ${_stage_cmds}
      COMMAND ${ASC_EXECUTABLE}
              ${_entry_src}
              --target release
              -Osize
              --runtime stub
              --noAssert
              ${_initial_memory_args}
              --outFile ${ARG_OUTPUT_WASM}
      DEPENDS ${ARG_SRC} ${LIBC_DIR}/assemblyscript/wasmos.ts
              ${LIBC_DIR}/assemblyscript/runtime.ts ${ARG_EXTRA_STAGE_FILES}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Building AssemblyScript ${ARG_NAME} module"
      VERBATIM
    )

    add_custom_command(
      OUTPUT ${ARG_OUTPUT_APP}
      COMMAND ${WASMOS_APP_PACKER}
              --manifest ${ARG_MANIFEST}
              --in ${ARG_OUTPUT_WASM}
              --out ${ARG_OUTPUT_APP}
      DEPENDS ${ARG_OUTPUT_WASM} make_wasmos_app ${ARG_MANIFEST}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Packing AssemblyScript ${ARG_NAME} as WASMOS-APP"
      VERBATIM
    )
  endfunction()

  wasmos_add_assemblyscript_app(
    NAME hello
    SRC ${AS_HELLO_SRC}
    OUTPUT_WASM ${AS_HELLO_WASM}
    OUTPUT_APP ${AS_HELLO_APP}
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  )

  wasmos_add_assemblyscript_app(
    NAME minesweeper
    SRC ${AS_MINESWEEPER_SRC}
    OUTPUT_WASM ${AS_MINESWEEPER_WASM}
    OUTPUT_APP ${AS_MINESWEEPER_APP}
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/minesweeper/linker.metadata
    INITIAL_MEMORY_PAGES 64
    EXTRA_STAGE_FILES
      ${CMAKE_SOURCE_DIR}/src/libui/assemblyscript/libui.ts
  )

  add_custom_target(assemblyscript_examples DEPENDS ${AS_HELLO_APP} ${AS_MINESWEEPER_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${AS_HELLO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${AS_MINESWEEPER_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS assemblyscript_examples)
endif ()
