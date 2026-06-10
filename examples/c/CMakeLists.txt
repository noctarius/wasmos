wasmos_add_wasm_c_app_target(hello_c
  SOURCE      ${WASM_HELLO_C_SRC}
  OUTPUT_WASM ${WASM_HELLO_C_WASM}
  OUTPUT_APP  ${WASM_HELLO_C_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/hello/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm hello-c module"
  PACK_COMMENT  "Packing wasm hello-c as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(chardev_client
  SOURCE      ${WASM_CHARDEV_CLIENT_SRC}
  OUTPUT_WASM ${WASM_CHARDEV_CLIENT_WASM}
  OUTPUT_APP  ${WASM_CHARDEV_CLIENT_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/chardev_client/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm chardev client module"
  PACK_COMMENT  "Packing wasm chardev client as WASMOS-APP"
)

add_custom_command(
  OUTPUT ${WASM_CHARDEV_CLIENT_BLOB}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          chardev_client.wasm chardev_client_blob.o
  DEPENDS ${WASM_CHARDEV_CLIENT_WASM}
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Embedding wasm chardev client module"
  VERBATIM
)
add_custom_target(chardev_client_blob DEPENDS ${WASM_CHARDEV_CLIENT_BLOB})

wasmos_add_wasm_c_app_target(chardev_preempt
  SOURCE      ${WASM_CHARDEV_PREEMPT_SRC}
  OUTPUT_WASM ${WASM_CHARDEV_PREEMPT_WASM}
  OUTPUT_APP  ${WASM_CHARDEV_PREEMPT_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/chardev_preempt/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm chardev preempt module"
  PACK_COMMENT  "Packing wasm chardev preempt as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(init_smoke
  SOURCE      ${WASM_INIT_SMOKE_SRC}
  OUTPUT_WASM ${WASM_INIT_SMOKE_WASM}
  OUTPUT_APP  ${WASM_INIT_SMOKE_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/init_smoke/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm init smoke module"
  PACK_COMMENT  "Packing wasm init smoke as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(native_call_smoke
  SOURCE      ${WASM_NATIVE_CALL_SMOKE_SRC}
  OUTPUT_WASM ${WASM_NATIVE_CALL_SMOKE_WASM}
  OUTPUT_APP  ${WASM_NATIVE_CALL_SMOKE_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/native_call_smoke/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm native call smoke module"
  PACK_COMMENT  "Packing wasm native call smoke as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(gfx_smoke
  SOURCE         ${WASM_GFX_SMOKE_SRC}
  OUTPUT_WASM    ${WASM_GFX_SMOKE_WASM}
  OUTPUT_APP     ${WASM_GFX_SMOKE_APP}
  MANIFEST       ${CMAKE_CURRENT_SOURCE_DIR}/gfx_smoke/linker.metadata
  EXPORT         wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  INITIAL_MEMORY 8388608
  MAX_MEMORY     8388608
  BUILD_COMMENT  "Building wasm gfx smoke module"
  PACK_COMMENT   "Packing wasm gfx smoke as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(fs_open_smoke
  SOURCE      ${WASM_FS_OPEN_SMOKE_SRC}
  OUTPUT_WASM ${WASM_FS_OPEN_SMOKE_WASM}
  OUTPUT_APP  ${WASM_FS_OPEN_SMOKE_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/fs_open_smoke/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm fs open smoke module"
  PACK_COMMENT  "Packing wasm fs open smoke as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(fs_write_smoke
  SOURCE      ${WASM_FS_WRITE_SMOKE_SRC}
  OUTPUT_WASM ${WASM_FS_WRITE_SMOKE_WASM}
  OUTPUT_APP  ${WASM_FS_WRITE_SMOKE_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/fs_write_smoke/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm fs write smoke module"
  PACK_COMMENT  "Packing wasm fs write smoke as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(native_call_min
  SOURCE      ${WASM_NATIVE_CALL_MIN_SRC}
  OUTPUT_WASM ${WASM_NATIVE_CALL_MIN_WASM}
  OUTPUT_APP  ${WASM_NATIVE_CALL_MIN_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/native_call_min/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm native call min module"
  PACK_COMMENT  "Packing wasm native call min as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(irq_route_deny
  SOURCE      ${WASM_IRQ_ROUTE_DENY_SRC}
  OUTPUT_WASM ${WASM_IRQ_ROUTE_DENY_WASM}
  OUTPUT_APP  ${WASM_IRQ_ROUTE_DENY_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/irq_route_deny/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm irq-route-deny module"
  PACK_COMMENT  "Packing wasm irq-route-deny as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(irq_route_allow
  SOURCE      ${WASM_IRQ_ROUTE_ALLOW_SRC}
  OUTPUT_WASM ${WASM_IRQ_ROUTE_ALLOW_WASM}
  OUTPUT_APP  ${WASM_IRQ_ROUTE_ALLOW_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/irq_route_allow/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm irq-route-allow module"
  PACK_COMMENT  "Packing wasm irq-route-allow as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(shmem_owner
  SOURCE      ${WASM_SHMEM_OWNER_SRC}
  OUTPUT_WASM ${WASM_SHMEM_OWNER_WASM}
  OUTPUT_APP  ${WASM_SHMEM_OWNER_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/shmem_owner/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm shmem-owner module"
  PACK_COMMENT  "Packing wasm shmownr as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(shmem_target
  SOURCE      ${WASM_SHMEM_TARGET_SRC}
  OUTPUT_WASM ${WASM_SHMEM_TARGET_WASM}
  OUTPUT_APP  ${WASM_SHMEM_TARGET_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/shmem_target/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  BUILD_COMMENT "Building wasm shmem-target module"
  PACK_COMMENT  "Packing wasm shmtgt as WASMOS-APP"
)

wasmos_add_wasm_c_app_target(menu_bar
  SOURCE         ${WASM_MENU_BAR_SRC}
  OUTPUT_WASM    ${WASM_MENU_BAR_WASM}
  OUTPUT_APP     ${WASM_MENU_BAR_APP}
  MANIFEST       ${CMAKE_CURRENT_SOURCE_DIR}/menu_bar/linker.metadata
  EXPORT         wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
  INITIAL_MEMORY 4194304
  MAX_MEMORY     4194304
  BUILD_COMMENT  "Building wasm menu-bar module"
  PACK_COMMENT   "Packing wasm menu-bar as WASMOS-APP"
)

add_custom_target(c_examples)
add_dependencies(c_examples
  hello_c chardev_client chardev_preempt init_smoke
  fs_open_smoke fs_write_smoke native_call_min native_call_smoke
  gfx_smoke irq_route_deny irq_route_allow shmem_owner shmem_target
  menu_bar
)

set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${WASM_CHARDEV_CLIENT_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS chardev_client_blob)
