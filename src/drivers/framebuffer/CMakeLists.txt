set(NATIVE_FB_CFLAGS
    --target=x86_64-elf
    -ffreestanding -fno-builtin -fno-stack-protector
    -fno-pic -fno-pie
    -mno-red-zone
    -mcmodel=small
    -Oz -c
    -Wall -Wextra
    -I${DRIVER_WASM_DIR}/include
    -I${DRIVER_WASM_DIR}/framebuffer
)

add_custom_command(
  OUTPUT ${NATIVE_FRAMEBUFFER_DRIVER_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NATIVE_FB_CFLAGS}
          ${NATIVE_FRAMEBUFFER_DRIVER_SRC}
          -o ${NATIVE_FRAMEBUFFER_DRIVER_OBJ}
  DEPENDS ${NATIVE_FRAMEBUFFER_DRIVER_SRC}
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/framebuffer/fbtext_internal.h
          ${DRIVER_WASM_DIR}/framebuffer/font_8x16.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling framebuffer_native.c"
  VERBATIM
)

add_custom_command(
  OUTPUT ${NATIVE_FRAMEBUFFER_RENDER_OBJ}
  COMMAND ${CLANG} ${NATIVE_FB_CFLAGS}
          ${NATIVE_FRAMEBUFFER_RENDER_SRC}
          -o ${NATIVE_FRAMEBUFFER_RENDER_OBJ}
  DEPENDS ${NATIVE_FRAMEBUFFER_RENDER_SRC}
          ${DRIVER_WASM_DIR}/framebuffer/fbtext_internal.h
          ${DRIVER_WASM_DIR}/framebuffer/font_8x16.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling render.c"
  VERBATIM
)

add_custom_command(
  OUTPUT ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
  COMMAND ${LLD_PATH}
          --image-base=0x10000000
          -e initialize
          -static
          ${NATIVE_FRAMEBUFFER_DRIVER_OBJ}
          ${NATIVE_FRAMEBUFFER_RENDER_OBJ}
          -o ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
  DEPENDS ${NATIVE_FRAMEBUFFER_DRIVER_OBJ}
          ${NATIVE_FRAMEBUFFER_RENDER_OBJ}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Linking native framebuffer driver"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FRAMEBUFFER_APP}
  COMMAND ${WASMOS_APP_PACKER}
          --manifest ${DRIVER_WASM_DIR}/framebuffer/linker.manifest
          --in ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
          --out ${WASM_FRAMEBUFFER_APP}
  DEPENDS ${NATIVE_FRAMEBUFFER_DRIVER_ELF} make_wasmos_app
          ${DRIVER_WASM_DIR}/framebuffer/linker.manifest
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing native framebuffer driver as WASMOS-APP"
  VERBATIM
)

add_custom_target(framebuffer_app DEPENDS ${WASM_FRAMEBUFFER_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_FRAMEBUFFER_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS framebuffer_app)
