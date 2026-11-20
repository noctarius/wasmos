wasmos_add_wasm_c_app_target(svc_util
  SOURCE      ${WASM_SVC_SRC}
  OUTPUT_WASM ${WASM_SVC_WASM}
  OUTPUT_APP  ${WASM_SVC_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  BUILD_COMMENT "Building wasm svc utility"
  PACK_COMMENT  "Packing wasm svc as WASMOS-APP"
)
