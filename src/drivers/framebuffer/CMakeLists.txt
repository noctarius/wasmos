set(NATIVE_FB_CFLAGS
    --target=x86_64-elf
    -ffreestanding -fno-builtin -fno-stack-protector
    -fno-pic -fno-pie
    -mno-red-zone
    -mcmodel=small
    -Oz -c
    -Wall -Wextra
)

wasmos_add_native_c_app_target(framebuffer_app
  SOURCES
    ${NATIVE_FRAMEBUFFER_DRIVER_SRC}
    ${NATIVE_FRAMEBUFFER_RENDER_SRC}
  OUTPUT_ELF ${NATIVE_FRAMEBUFFER_DRIVER_ELF}
  OUTPUT_APP ${WASM_FRAMEBUFFER_APP}
  MANIFEST ${DRIVER_WASM_DIR}/framebuffer/linker.metadata
  ENTRYPOINT initialize
  IMAGE_BASE 0x10000000
  COMPILE_FLAGS ${NATIVE_FB_CFLAGS}
  INCLUDES ${DRIVER_WASM_DIR}/include ${DRIVER_WASM_DIR}/framebuffer
  HEADER_DEPS
    ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
    ${DRIVER_WASM_DIR}/framebuffer/fbtext_internal.h
    ${DRIVER_WASM_DIR}/framebuffer/font_8x16.h
  LINK_COMMENT "Linking native framebuffer driver"
  PACK_COMMENT "Packing native framebuffer driver as WASMOS-APP"
)
