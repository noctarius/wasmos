if (AS_ENABLE)
  set(WASM_SCHED_INFO_LIBC_SRC ${LIBC_DIR}/assemblyscript/wasmos.ts)
  set(WASM_SCHED_INFO_RUNTIME_SRC ${LIBC_DIR}/assemblyscript/runtime.ts)
  set(WASM_SCHED_INFO_STAGE_DIR ${BUILD_DIR}/assemblyscript_sched_info_src)
  set(WASM_SCHED_INFO_ENTRY_SRC ${WASM_SCHED_INFO_STAGE_DIR}/entry.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found.")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_SCHED_INFO_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_SCHED_INFO_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_SCHED_INFO_SRC} ${WASM_SCHED_INFO_STAGE_DIR}/app.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_SCHED_INFO_LIBC_SRC} ${WASM_SCHED_INFO_STAGE_DIR}/wasmos.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_SCHED_INFO_RUNTIME_SRC} ${WASM_SCHED_INFO_ENTRY_SRC}
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_SCHED_INFO_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_SCHED_INFO_WASM}
    DEPENDS ${WASM_SCHED_INFO_SRC} ${WASM_SCHED_INFO_LIBC_SRC} ${WASM_SCHED_INFO_RUNTIME_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript sched_info utility"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_SCHED_INFO_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
            --in ${WASM_SCHED_INFO_WASM}
            --out ${WASM_SCHED_INFO_APP}
    DEPENDS ${WASM_SCHED_INFO_WASM} make_wasmos_app
            ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing sched_info as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(sched_info_util DEPENDS ${WASM_SCHED_INFO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_SCHED_INFO_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS sched_info_util)
endif ()
