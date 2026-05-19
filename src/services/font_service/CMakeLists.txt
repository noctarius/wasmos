if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
  unset(ZIG_EXECUTABLE CACHE)
  find_program(ZIG_EXECUTABLE zig HINTS
    ${CLANG_BIN_DIR}
    $ENV{HOME}/bin
    $ENV{HOME}/.local/bin
  )
endif ()

if (NOT ZIG_EXECUTABLE)
  message(FATAL_ERROR "zig is required to build native font-service")
endif ()

set(FONT_SERVICE_ZIG_OBJ ${BUILD_DIR}/font_service_zig.o)
set(FONT_SERVICE_ELF ${WASM_FONT_SERVICE_WASM})

add_custom_command(
  OUTPUT ${FONT_SERVICE_ZIG_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${ZIG_EXECUTABLE}
          build-obj
          -target x86_64-freestanding
          -O ReleaseSmall
          -fstrip
          -I ${CMAKE_SOURCE_DIR}
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -femit-bin=${FONT_SERVICE_ZIG_OBJ}
          ${WASM_FONT_SERVICE_SRC}
  DEPENDS ${WASM_FONT_SERVICE_SRC}
          ${CMAKE_CURRENT_SOURCE_DIR}/font_service_imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
          ${LIBC_DIR}/include/wasmos/font_ipc.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building native Zig font-service object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_ELF}
  COMMAND ${LLD_PATH}
          -e initialize
          -static
          ${FONT_SERVICE_ZIG_OBJ}
          -o ${FONT_SERVICE_ELF}
  DEPENDS ${FONT_SERVICE_ZIG_OBJ}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Linking native Zig font-service"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FONT_SERVICE_APP}
  COMMAND ${WASMOS_APP_PACKER}
          --manifest ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
          --in ${FONT_SERVICE_ELF}
          --out ${WASM_FONT_SERVICE_APP}
  DEPENDS ${FONT_SERVICE_ELF} make_wasmos_app ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing native Zig font-service as WASMOS-APP"
  VERBATIM
)

add_custom_target(font_service_app
  DEPENDS ${WASM_FONT_SERVICE_APP}
  SOURCES ${WASM_FONT_SERVICE_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
)

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_FONT_SERVICE_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS font_service_app)
