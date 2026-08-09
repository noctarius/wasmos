include(${CMAKE_SOURCE_DIR}/cmake/WasmosAssemblyScript.cmake)

if (AS_ENABLE)
  wasmos_assemblyscript_compile(
    NAME mouse
    ENTRY ${WASM_MOUSE_DRIVER_SRC}
    OUTPUT_WASM ${WASM_MOUSE_DRIVER_WASM}
  )

  wasmos_maybe_aot_pack(
    "${DRIVER_WASM_DIR}/mouse/linker.metadata"
    "${WASM_MOUSE_DRIVER_WASM}"
    "${WASM_MOUSE_APP}"
  )

  add_custom_target(mouse_app
    DEPENDS ${WASM_MOUSE_APP}
    SOURCES ${WASM_MOUSE_DRIVER_SRC} ${DRIVER_WASM_DIR}/mouse/linker.metadata
  )
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_MOUSE_APP})
  set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS mouse_app)
endif ()
