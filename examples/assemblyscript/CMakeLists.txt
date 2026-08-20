include(${CMAKE_SOURCE_DIR}/cmake/WasmosAssemblyScript.cmake)

if (AS_ENABLE)
  # Apps enter through libc's runtime.ts, which imports the app's own module as
  # "./app" -- so the entry staged into the build is runtime.ts and the app
  # source is staged beside it under that name.
  function(wasmos_add_assemblyscript_app)
    cmake_parse_arguments(ARG "" "NAME;SRC;OUTPUT_WASM;OUTPUT_APP;MANIFEST"
                          "EXTRA_STAGE_FILES" ${ARGN})

    if (NOT ARG_NAME OR NOT ARG_SRC OR NOT ARG_OUTPUT_WASM OR NOT ARG_OUTPUT_APP OR NOT ARG_MANIFEST)
      message(FATAL_ERROR "wasmos_add_assemblyscript_app: missing required argument")
    endif ()

    wasmos_assemblyscript_compile(
      NAME ${ARG_NAME}
      ENTRY ${LIBC_DIR}/assemblyscript/runtime.ts
      OUTPUT_WASM ${ARG_OUTPUT_WASM}
      MANIFEST ${ARG_MANIFEST}
      EXTRA_SOURCES ${ARG_SRC} ${ARG_EXTRA_STAGE_FILES}
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
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/hello/linker.metadata
  )

  wasmos_add_assemblyscript_app(
    NAME minesweeper
    SRC ${AS_MINESWEEPER_SRC}
    OUTPUT_WASM ${AS_MINESWEEPER_WASM}
    OUTPUT_APP ${AS_MINESWEEPER_APP}
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/minesweeper/linker.metadata
    EXTRA_STAGE_FILES
      ${CMAKE_SOURCE_DIR}/src/libui/assemblyscript/libui.ts
  )

  add_custom_target(assemblyscript_examples DEPENDS ${AS_HELLO_APP} ${AS_MINESWEEPER_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${AS_HELLO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${AS_MINESWEEPER_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS assemblyscript_examples)
endif ()
