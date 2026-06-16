if (AS_ENABLE)
  set(WASM_DATE_TOOL_LIBC_SRC ${LIBC_DIR}/assemblyscript/wasmos.ts)
  set(WASM_DATE_TOOL_RUNTIME_SRC ${LIBC_DIR}/assemblyscript/runtime.ts)
  set(WASM_DATE_TOOL_STAGE_DIR ${BUILD_DIR}/assemblyscript_date_tool_src)
  set(WASM_DATE_TOOL_ENTRY_SRC ${WASM_DATE_TOOL_STAGE_DIR}/entry.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_DATE_TOOL_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_DATE_TOOL_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_DATE_TOOL_SRC} ${WASM_DATE_TOOL_STAGE_DIR}/app.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_DATE_TOOL_LIBC_SRC} ${WASM_DATE_TOOL_STAGE_DIR}/wasmos.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_DATE_TOOL_RUNTIME_SRC} ${WASM_DATE_TOOL_ENTRY_SRC}
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_DATE_TOOL_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_DATE_TOOL_WASM}
    DEPENDS ${WASM_DATE_TOOL_SRC} ${WASM_DATE_TOOL_LIBC_SRC} ${WASM_DATE_TOOL_RUNTIME_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript date tool module"
    VERBATIM
  )

  wasmos_maybe_aot_pack(
    "${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata"
    "${WASM_DATE_TOOL_WASM}"
    "${WASM_DATE_TOOL_APP}"
  )

  add_custom_target(system_utils DEPENDS ${WASM_DATE_TOOL_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_DATE_TOOL_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS system_utils)
endif ()
