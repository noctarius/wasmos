# FLAG_DRIVER=1, FLAG_NATIVE=16 -> 17
set(NATIVE_FRAMEBUFFER_FLAGS 17)
set(NATIVE_FRAMEBUFFER_OBJ ${BUILD_DIR}/framebuffer_native.o)

add_custom_command(
  OUTPUT ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  # Compile to object
  COMMAND ${CLANG}
          --target=x86_64-elf
          -ffreestanding -fno-builtin -fno-stack-protector
          -fno-pic -fno-pie
          -mno-red-zone
          -mcmodel=small
          -Oz -c
          -Wall -Wextra
          -I${DRIVER_WASM_DIR}/include
          ${NATIVE_FRAMEBUFFER_DRIVER_SRC}
          -o ${NATIVE_FRAMEBUFFER_OBJ}
  # Link with lld directly (same approach as the kernel build)
  COMMAND ${LLD_PATH}
          --image-base=0x10000000
          -e initialize
          -static
          ${NATIVE_FRAMEBUFFER_OBJ}
          -o ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
  DEPENDS ${NATIVE_FRAMEBUFFER_DRIVER_SRC}
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building native framebuffer driver"
  VERBATIM
)

add_custom_command(
  OUTPUT ${WASM_FRAMEBUFFER_APP}
  COMMAND ${WASMOS_APP_PACKER}
          ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
          ${WASM_FRAMEBUFFER_APP}
          framebuffer
          initialize
          0
          0
          ${NATIVE_FRAMEBUFFER_FLAGS}
          -
          0
          -
          0
  DEPENDS ${NATIVE_FRAMEBUFFER_DRIVER_ELF} make_wasmos_app
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing native framebuffer driver as WASMOS-APP"
  VERBATIM
)

add_custom_target(framebuffer_app DEPENDS ${WASM_FRAMEBUFFER_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_FRAMEBUFFER_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS framebuffer_app)
