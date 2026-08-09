include(${CMAKE_SOURCE_DIR}/cmake/WasmosAssemblyScript.cmake)

if (AS_ENABLE)
  wasmos_assemblyscript_compile(
    NAME rtc
    ENTRY ${WASM_RTC_DRIVER_SRC}
    OUTPUT_WASM ${WASM_RTC_DRIVER_WASM}
  )

  wasmos_maybe_aot_pack(
    "${DRIVER_WASM_DIR}/rtc/linker.metadata"
    "${WASM_RTC_DRIVER_WASM}"
    "${WASM_RTC_APP}"
  )

  add_custom_target(rtc_app
    DEPENDS ${WASM_RTC_APP}
    SOURCES ${WASM_RTC_DRIVER_SRC} ${DRIVER_WASM_DIR}/rtc/linker.metadata
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_RTC_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS rtc_app)
endif ()
