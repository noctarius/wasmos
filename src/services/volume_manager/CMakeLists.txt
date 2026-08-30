include(${CMAKE_SOURCE_DIR}/cmake/WasmosZigApp.cmake)

# src/services is configured before examples/zig, which is where ZIG_AVAILABLE is
# set, so the probe has to be repeated here rather than read from the parent.
# The result is exported so src/services/CMakeLists.txt can decide whether the
# `services` aggregate target may depend on a target that was never created.
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
    set(VOLUME_MANAGER_ZIG_AVAILABLE ON PARENT_SCOPE)
  endif ()
endif ()

# EXTRA_SRCS are staged flat beside the service so Zig's @import resolves them:
#   - the driver-side shim, which carries the block and volume descriptor
#     mirrors, the class-registry helpers and the transfer-buffer wrappers;
#   - the coroutine runtime and its async-service entry, which owns this
#     service's event loop and pump (the manifest entry is async_initialize, so
#     service_async_entry_wasm.c is what the loader actually calls);
#   - the recognisers, and partition_table.zig beneath them: recognition and
#     table detection read the same prefix, so they are one library here exactly
#     as libblkid keeps `superblocks/` beside `partitions/`. The parser is shared
#     with the partition manager rather than copied, so the two components cannot
#     disagree about what a GPT is;
#   - the generated ABI, so opcodes, packed error codes and value constants come
#     from abi/*.yaml rather than being restated here.
#
# No vring shim and no DMA: this service reaches every device over IPC and drives
# no hardware.
wasmos_add_zig_wasm_app(
  NAME        volume_manager
  TARGET      volume_manager_app
  SRC         ${WASM_VOLUME_MANAGER_SRC}
  OUTPUT_WASM ${WASM_VOLUME_MANAGER_WASM}
  OUTPUT_APP  ${WASM_VOLUME_MANAGER_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXTRA_SRCS
    ${LIBC_DIR}/zig/driver.zig
    ${LIBC_DIR}/zig/coroutine.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/recognise.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/recognise_fat.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/recognise_wfs.zig
    ${DRIVER_WASM_DIR}/partition_manager/partition_table.zig
    ${LIBSYS_WASM_DIR}/coroutine_wasm.c
    ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
    ${LIBSYS_WASM_DIR}/service_runtime_wasm.c
    ${LIBSYS_WASM_DIR}/service_async_entry_wasm.c
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_imports.zig
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_opcodes.zig
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_status.zig
    ${CMAKE_SOURCE_DIR}/abi/generated/zig/wasmos_constants.zig
)
