include(${CMAKE_SOURCE_DIR}/cmake/WasmosAssemblyScript.cmake)

if (AS_ENABLE)
  wasmos_assemblyscript_compile(
    NAME keyboard
    ENTRY ${WASM_KEYBOARD_DRIVER_SRC}
    OUTPUT_WASM ${WASM_KEYBOARD_DRIVER_WASM}
  )

  wasmos_maybe_aot_pack(
    "${DRIVER_WASM_DIR}/keyboard/linker.metadata"
    "${WASM_KEYBOARD_DRIVER_WASM}"
    "${WASM_KEYBOARD_APP}"
  )

  add_custom_target(keyboard_app
    DEPENDS ${WASM_KEYBOARD_APP}
    SOURCES ${WASM_KEYBOARD_DRIVER_SRC} ${DRIVER_WASM_DIR}/keyboard/linker.metadata
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_KEYBOARD_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS keyboard_app)
endif ()
