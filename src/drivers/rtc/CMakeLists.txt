if (AS_ENABLE)
  set(WASM_RTC_STAGE_DIR ${BUILD_DIR}/rtc_src)
  set(WASM_RTC_ENTRY_SRC ${WASM_RTC_STAGE_DIR}/rtc.ts)
  find_program(ASC_EXECUTABLE asc HINTS ${CLANG_BIN_DIR})
  if (NOT ASC_EXECUTABLE)
    message(FATAL_ERROR "AssemblyScript compiler 'asc' not found. Install with: npm i -g assemblyscript")
  endif ()

  add_custom_command(
    OUTPUT ${WASM_RTC_DRIVER_WASM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WASM_RTC_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WASM_RTC_DRIVER_SRC} ${WASM_RTC_STAGE_DIR}/rtc.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBC_DIR}/assemblyscript/wasmos.ts ${WASM_RTC_STAGE_DIR}/wasmos.ts
    COMMAND ${ASC_EXECUTABLE}
            ${WASM_RTC_ENTRY_SRC}
            --target release
            -Osize
            --runtime stub
            --noAssert
            --outFile ${WASM_RTC_DRIVER_WASM}
    DEPENDS ${WASM_RTC_DRIVER_SRC} ${LIBC_DIR}/assemblyscript/wasmos.ts
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building AssemblyScript rtc driver module"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${WASM_RTC_APP}
    COMMAND ${WASMOS_APP_PACKER}
            --manifest ${DRIVER_WASM_DIR}/rtc/linker.metadata
            --in ${WASM_RTC_DRIVER_WASM}
            --out ${WASM_RTC_APP}
    DEPENDS ${WASM_RTC_DRIVER_WASM} make_wasmos_app
            ${DRIVER_WASM_DIR}/rtc/linker.metadata
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packing rtc driver as WASMOS-APP"
    VERBATIM
  )

  add_custom_target(rtc_app
    DEPENDS ${WASM_RTC_APP}
    SOURCES ${WASM_RTC_DRIVER_SRC} ${DRIVER_WASM_DIR}/rtc/linker.metadata
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_RTC_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS rtc_app)
endif ()
