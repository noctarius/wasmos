# ipc_future_wasm.c: the CLI drives its VT round-trips as IPC futures resolved by
# one owned event-loop pump, instead of per-call drain loops that discarded every
# message they were not waiting for.  coroutine_wasm.c is already in
# WASMOS_LIBC_SOURCES for every wasm C app.
wasmos_add_wasm_c_app_target(cli_app
  SOURCES ${WASM_CLI_APP_SRC} ${WASMOS_SCRIPT_ENGINE_SRC}
          ${LIBSYS_WASM_DIR}/ipc_future_wasm.c
  OUTPUT_WASM ${WASM_CLI_APP_WASM}
  OUTPUT_APP ${WASM_CLI_APP}
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT initialize
  STACK_SIZE 8192
  INITIAL_MEMORY 131072
  MAX_MEMORY 131072
  BUILD_COMMENT "Building wasm CLI module"
  PACK_COMMENT "Packing wasm CLI as WASMOS-APP"
)
