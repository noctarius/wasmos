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
set(FONT_SERVICE_LIBSYS_EXPORTS_OBJ ${BUILD_DIR}/font_service_libsys_exports.o)
set(FONT_SERVICE_STB_OBJ ${BUILD_DIR}/font_service_stb.o)
set(FONT_SERVICE_LIBC_CTYPE_OBJ ${BUILD_DIR}/font_service_libc_ctype.o)
set(FONT_SERVICE_LIBC_STRING_OBJ ${BUILD_DIR}/font_service_libc_string.o)
set(FONT_SERVICE_LIBC_MATH_OBJ ${BUILD_DIR}/font_service_libc_math.o)
set(FONT_SERVICE_ELF ${WASM_FONT_SERVICE_WASM})

add_custom_command(
  OUTPUT ${FONT_SERVICE_ZIG_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${ZIG_EXECUTABLE}
          build-obj
          -target x86_64-freestanding
          -O ReleaseSmall
          -fstrip
          --dep libsys
          -I ${CMAKE_SOURCE_DIR}
          -I ${CMAKE_SOURCE_DIR}/libs/stb
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -Mroot=${WASM_FONT_SERVICE_SRC}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -Mlibsys=${LIBSYS_NATIVE_DIR}/zig/libsys.zig
          -femit-bin=${FONT_SERVICE_ZIG_OBJ}
  DEPENDS ${WASM_FONT_SERVICE_SRC}
          ${LIBSYS_NATIVE_DIR}/zig/libsys.zig
          ${LIBSYS_NATIVE_DIR}/zig/c_abi.zig
          ${CMAKE_CURRENT_SOURCE_DIR}/zig_utils.zig
          ${CMAKE_CURRENT_SOURCE_DIR}/font_service_imports.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
          ${LIBC_DIR}/include/wasmos/font_ipc.h
          ${CMAKE_SOURCE_DIR}/libs/stb/stb_truetype.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building native Zig font-service object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_LIBSYS_EXPORTS_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBSYS_NATIVE_DIR}/include
          -c ${LIBSYS_NATIVE_DIR}/zig/libsys_exports.c
          -o ${FONT_SERVICE_LIBSYS_EXPORTS_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/zig/libsys_exports.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native_exports.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building font-service libsys exports object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_STB_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${CMAKE_SOURCE_DIR}
          -I ${CMAKE_SOURCE_DIR}/libs/stb
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          -I ${DRIVER_WASM_DIR}/include
          -I ${LIBC_DIR}/include
          -c ${CMAKE_CURRENT_SOURCE_DIR}/stb_truetype_impl.c
          -o ${FONT_SERVICE_STB_OBJ}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/stb_truetype_impl.c
          ${CMAKE_CURRENT_SOURCE_DIR}/font_service_imports.h
          ${CMAKE_SOURCE_DIR}/libs/stb/stb_truetype.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building font-service stb_truetype implementation object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_LIBC_CTYPE_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/ctype.c
          -o ${FONT_SERVICE_LIBC_CTYPE_OBJ}
  DEPENDS ${LIBC_DIR}/src/ctype.c ${LIBC_DIR}/include/ctype.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building font-service libc ctype object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_LIBC_STRING_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/string.c
          -o ${FONT_SERVICE_LIBC_STRING_OBJ}
  DEPENDS ${LIBC_DIR}/src/string.c ${LIBC_DIR}/include/string.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building font-service libc string object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_LIBC_MATH_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG}
          -target ${KERNEL_TARGET_TRIPLE}
          -ffreestanding
          -fno-stack-protector
          -fno-pic
          -fno-pie
          -fno-builtin
          -nostdlib
          -O2
          -I ${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/math.c
          -o ${FONT_SERVICE_LIBC_MATH_OBJ}
  DEPENDS ${LIBC_DIR}/src/math.c ${LIBC_DIR}/include/math.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building font-service libc math object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${FONT_SERVICE_ELF}
  COMMAND ${LLD_PATH}
          -e initialize
          -static
          ${FONT_SERVICE_ZIG_OBJ}
          ${FONT_SERVICE_LIBSYS_EXPORTS_OBJ}
          ${FONT_SERVICE_STB_OBJ}
          ${FONT_SERVICE_LIBC_CTYPE_OBJ}
          ${FONT_SERVICE_LIBC_STRING_OBJ}
          ${FONT_SERVICE_LIBC_MATH_OBJ}
          -o ${FONT_SERVICE_ELF}
  DEPENDS ${FONT_SERVICE_ZIG_OBJ}
          ${FONT_SERVICE_LIBSYS_EXPORTS_OBJ}
          ${FONT_SERVICE_STB_OBJ}
          ${FONT_SERVICE_LIBC_CTYPE_OBJ}
          ${FONT_SERVICE_LIBC_STRING_OBJ}
          ${FONT_SERVICE_LIBC_MATH_OBJ}
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
