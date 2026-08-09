include(${CMAKE_SOURCE_DIR}/cmake/WasmosAssemblyScript.cmake)

if (AS_ENABLE)
  wasmos_assemblyscript_compile(
    NAME serial
    ENTRY ${WASM_SERIAL_DRIVER_SRC}
    OUTPUT_WASM ${WASM_SERIAL_DRIVER_WASM}
  )

  wasmos_maybe_aot_pack(
    "${DRIVER_WASM_DIR}/serial/linker.metadata"
    "${WASM_SERIAL_DRIVER_WASM}"
    "${WASM_SERIAL_APP}"
  )

  add_custom_target(serial_app
    DEPENDS ${WASM_SERIAL_APP}
    SOURCES ${WASM_SERIAL_DRIVER_SRC} ${DRIVER_WASM_DIR}/serial/linker.metadata
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_SERIAL_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS serial_app)
endif ()
