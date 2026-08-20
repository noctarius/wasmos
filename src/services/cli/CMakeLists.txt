# ipc_future_wasm.c: the CLI drives its VT round-trips as IPC futures resolved by
# one owned event-loop pump, rather than per-call drain loops that discard every
# message they are not waiting for.  It is named here, not left to libsys.a,
# because the CLI is the only caller and the archive would pull it anyway.
wasmos_add_wasm_c_app_target(cli_app
  SOURCES ${WASM_CLI_APP_SRC}
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
