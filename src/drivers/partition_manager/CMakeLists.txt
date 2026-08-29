include(${CMAKE_SOURCE_DIR}/cmake/WasmosZigApp.cmake)

# src/drivers is configured before examples/zig, which is where ZIG_AVAILABLE is
# set, so the probe has to be repeated here rather than read from the parent.
# The result is exported so src/drivers/CMakeLists.txt can decide whether the
# `drivers` aggregate target may depend on a target that was never created.
if (ZIG_ENABLE)
  if (NOT ZIG_EXECUTABLE OR ZIG_EXECUTABLE MATCHES "NOTFOUND")
    unset(ZIG_EXECUTABLE CACHE)
    find_program(ZIG_EXECUTABLE zig HINTS
      ${CLANG_BIN_DIR}
      $ENV{HOME}/bin
      $ENV{HOME}/.local/bin
    )
  endif ()
  if (ZIG_EXECUTABLE)
    set(PARTITION_MANAGER_ZIG_AVAILABLE ON PARENT_SCOPE)
  endif ()
endif ()

# EXTRA_SRCS are staged flat beside the driver so Zig's @import resolves them:
#   - the driver-side shim, which carries the block descriptor and request
#     mirrors, the class-registry helpers and the transfer-buffer wrappers;
#   - the coroutine runtime and its async-service entry, which owns this
#     driver's event loop and pump (the manifest entry is async_initialize, so
#     service_async_entry_wasm.c is what the loader actually calls);
#   - partition_table.zig, the pure GPT/MBR parser, which is also compiled
#     standalone by the host unit tests;
#   - the generated ABI, so opcodes, packed error codes and value constants come
#     from abi/*.yaml rather than being restated here.
#
# No vring shim and no DMA: this driver reaches every disk over IPC and drives no
# device.
wasmos_add_zig_wasm_app(
  NAME        partition_manager
  TARGET      partition_manager_app
  SRC         ${WASM_PARTITION_MANAGER_DRIVER_SRC}
  OUTPUT_WASM ${WASM_PARTITION_MANAGER_DRIVER_WASM}
  OUTPUT_APP  ${WASM_PARTITION_MANAGER_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXTRA_SRCS
    ${LIBC_DIR}/zig/driver.zig
    ${LIBC_DIR}/zig/coroutine.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/partition_table.zig
    ${LIBSYS_WASM_DIR}/coroutine_wasm.c
    ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
    ${LIBSYS_WASM_DIR}/service_runtime_wasm.c
    ${LIBSYS_WASM_DIR}/service_async_entry_wasm.c
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_imports.zig
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_opcodes.zig
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_status.zig
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_constants.zig
)
