# wasmos_sdk_stage.cmake - run via "cmake -P" to populate the staged SDK's
# sysroot, wrappers, libexec and CMake integration.
#
# Header rewrite: src/libc/include/wasmos/api.h includes the generated ABI
# headers by a repo-relative path (../../../../abi/generated/c/...), which is
# deliberate in-tree -- the generated files live outside src/ to stay out of
# format/lint scope -- but no sysroot can reproduce that depth. The installed copy
# therefore includes <wasmos/abi/...> instead, and the rewrite is asserted rather
# than assumed: a silent miss would produce a sysroot that cannot compile
# anything.
cmake_minimum_required(VERSION 3.20)

foreach (_req IN ITEMS SRC_DIR SDK_DIR SYSROOT LIBC_DIR LIBSYS_WASM_DIR STAMP)
  if (NOT DEFINED ${_req})
    message(FATAL_ERROR "wasmos_sdk_stage: ${_req} must be defined")
  endif ()
endforeach ()

set(_inc ${SYSROOT}/include)
file(MAKE_DIRECTORY ${_inc} ${_inc}/wasmos ${_inc}/wasmos/abi ${_inc}/sys)
file(MAKE_DIRECTORY ${SDK_DIR}/bin ${SDK_DIR}/libexec/wasmos)
file(MAKE_DIRECTORY ${SDK_DIR}/share/wasmos ${SDK_DIR}/share/cmake/WASMOS/Platform)

# --- headers ---------------------------------------------------------------
file(GLOB _libc_top ${LIBC_DIR}/include/*.h)
foreach (_h IN LISTS _libc_top)
  file(COPY ${_h} DESTINATION ${_inc})
endforeach ()
file(GLOB _libc_sys ${LIBC_DIR}/include/sys/*.h)
foreach (_h IN LISTS _libc_sys)
  file(COPY ${_h} DESTINATION ${_inc}/sys)
endforeach ()
file(GLOB _wasmos_hdrs
  ${LIBC_DIR}/include/wasmos/*.h
  ${LIBSYS_WASM_DIR}/include/wasmos/*.h
  ${SRC_DIR}/src/libui/include/wasmos/*.h
)
foreach (_h IN LISTS _wasmos_hdrs)
  file(COPY ${_h} DESTINATION ${_inc}/wasmos)
endforeach ()
# src/drivers/include holds the headers wasmos/{ipc,proc,net}.h and libsys.h
# include by bare name -- wasmos_driver_abi.h above all, which every IPC-using app
# reaches transitively. They are part of the public surface, not a driver-only
# detail, and a sysroot without them compiles hello-world and nothing that talks
# to a service. tests/test_sdk_headers.py compiles the whole set to keep it that
# way.
file(GLOB _driver_hdrs ${DRIVER_INCLUDE_DIR}/*.h)
foreach (_h IN LISTS _driver_hdrs)
  file(COPY ${_h} DESTINATION ${_inc})
endforeach ()
file(GLOB _driver_wasmos_hdrs ${DRIVER_INCLUDE_DIR}/wasmos/*.h)
foreach (_h IN LISTS _driver_wasmos_hdrs)
  file(COPY ${_h} DESTINATION ${_inc}/wasmos)
endforeach ()

file(GLOB _abi_hdrs ${SRC_DIR}/abi/generated/c/*.h)
foreach (_h IN LISTS _abi_hdrs)
  file(COPY ${_h} DESTINATION ${_inc}/wasmos/abi)
endforeach ()

# Rewrite every repo-relative generated-ABI include to the sysroot layout.
#
# Six public headers reach abi/generated/c by relative path, at THREE different
# depths (wasmos/api.h and the *_ipc.h headers from libc, one deeper from libsys,
# one shallower from src/drivers/include). Rewriting api.h alone -- which is what
# this did first -- left wasmos_driver_abi.h broken, and with it every header that
# includes it: wasmos/{ipc,proc,net}.h and libsys.h, which is to say everything an
# app doing IPC needs. So the substitution is a regex over any depth, applied to
# every staged header, and then asserted: no staged header may still carry one.
file(GLOB_RECURSE _staged_hdrs ${_inc}/*.h)
foreach (_h IN LISTS _staged_hdrs)
  file(READ ${_h} _text)
  string(REGEX REPLACE "(\\.\\./)+abi/generated/c/" "wasmos/abi/" _rewritten "${_text}")
  if (NOT _rewritten STREQUAL _text)
    file(WRITE ${_h} "${_rewritten}")
  endif ()
endforeach ()

file(GLOB_RECURSE _staged_hdrs ${_inc}/*.h)
foreach (_h IN LISTS _staged_hdrs)
  file(READ ${_h} _text)
  # Match the relative INCLUDE, not the string: several of these headers mention
  # abi/generated/c in prose when citing where a generated header comes from, and
  # a bare substring check fails on the documentation.
  if (_text MATCHES "(\\.\\./)+abi/generated/c/")
    message(FATAL_ERROR
      "wasmos_sdk_stage: ${_h} still carries a repo-relative ABI include")
  endif ()
endforeach ()
if (NOT EXISTS ${_inc}/wasmos/abi/wasmos_imports.h)
  message(FATAL_ERROR "wasmos_sdk_stage: generated ABI headers missing from the sysroot")
endif ()

# --- host tools the wrappers front ----------------------------------------
if (DEFINED PACKER AND EXISTS ${PACKER})
  file(COPY ${PACKER} DESTINATION ${SDK_DIR}/libexec/wasmos
       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
endif ()
file(COPY ${SRC_DIR}/scripts/wasm_inspect.py DESTINATION ${SDK_DIR}/libexec/wasmos)
# The layout check wasmos-zig runs after every Zig build: a module whose globals
# sit above the user-VA mirror region fails host calls silently, so the driver
# refuses to emit one rather than leaving it to be discovered at runtime.
file(COPY ${SRC_DIR}/scripts/wasm_stack_check.py DESTINATION ${SDK_DIR}/libexec/wasmos)

# --- driver + tool wrappers ------------------------------------------------
set(_perm FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
foreach (_w IN ITEMS wasmos-clang wasmos-clang++ wasmos-zig wasmos-asc wasmos-rustc
                     wasmos-tinygo wasmos-pack wasmos-inspect)
  file(COPY ${SRC_DIR}/scripts/sdk/${_w} DESTINATION ${SDK_DIR}/bin ${_perm})
endforeach ()

# Thin execs for the LLVM binutils, so the SDK's own tools are used rather than
# whatever happens to be on PATH.
function(wasmos_sdk_tool name tool)
  if (NOT tool OR NOT EXISTS "${tool}")
    return()
  endif ()
  file(WRITE ${SDK_DIR}/bin/${name}
"#!/bin/sh
# ${name} - the SDK's ${tool} face. Generated by cmake/wasmos_sdk_stage.cmake.
exec \"${tool}\" \"$@\"
")
  file(CHMOD ${SDK_DIR}/bin/${name}
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
endfunction()

wasmos_sdk_tool(wasmos-ar "${SDK_AR}")
wasmos_sdk_tool(wasmos-nm "${SDK_NM}")
wasmos_sdk_tool(wasmos-ranlib "${SDK_RANLIB}")
wasmos_sdk_tool(wasmos-strip "${SDK_STRIP}")
wasmos_sdk_tool(wasmos-objdump "${SDK_OBJDUMP}")
wasmos_sdk_tool(wasmos-ld "${SDK_WASMLD}")

# --- share/ ----------------------------------------------------------------
# Per-language runtime shims are SOURCE, not headers or archives: Zig and
# AssemblyScript compile theirs together with the app, and Zig resolves
# @import("wasmos.zig") beside the importing file. They live under share/ because
# a sysroot's include/ and lib/ are what a C compiler searches, and these are
# neither.
file(MAKE_DIRECTORY ${SDK_DIR}/share/wasmos/zig)
foreach (_zig IN ITEMS wasmos.zig coroutine.zig)
  file(COPY ${LIBC_DIR}/zig/${_zig} DESTINATION ${SDK_DIR}/share/wasmos/zig)
endforeach ()

# AssemblyScript is staged the same way and for a sharper reason: asc has no
# include path and resolves every import relative to the entry file, so the
# runtime has to sit flat beside the app. The entry is runtime.ts, which imports
# "./app"; the driver stages the developer's file under that name. AS_SOURCES is
# the same list the in-tree build stages, passed in so the two cannot diverge.
file(MAKE_DIRECTORY ${SDK_DIR}/share/wasmos/assemblyscript)
file(COPY ${LIBC_DIR}/assemblyscript/runtime.ts
     DESTINATION ${SDK_DIR}/share/wasmos/assemblyscript)
foreach (_as IN LISTS AS_SOURCES)
  file(COPY ${_as} DESTINATION ${SDK_DIR}/share/wasmos/assemblyscript)
endforeach ()
if (EXISTS ${SRC_DIR}/tools/as_coroutine_transform.mjs)
  file(COPY ${SRC_DIR}/tools/as_coroutine_transform.mjs
       DESTINATION ${SDK_DIR}/libexec/wasmos)
endif ()

# Rust reaches its binding as a sibling module (`mod wasmos;`), so the two .rs
# files are staged beside a copy of the app the same way. Unlike Zig and
# AssemblyScript, the C entry points the Rust binding declares as extern "C" come
# from libsys.a in the sysroot, which rustc links through -C link-arg.
file(MAKE_DIRECTORY ${SDK_DIR}/share/wasmos/rust)
foreach (_rs IN ITEMS wasmos.rs coroutine.rs)
  file(COPY ${LIBC_DIR}/rust/${_rs} DESTINATION ${SDK_DIR}/share/wasmos/rust)
endforeach ()

# Go needs both halves staged: the Go binding, and the C shims a Go guest links.
# The C files are copies rather than references into src/ because TinyGo resolves
# a target's extra-files relative to TINYGOROOT, and the driver computes those
# relative paths from wherever these end up -- so they have to live inside the
# SDK, which is the tree that moves with the developer.
file(MAKE_DIRECTORY ${SDK_DIR}/share/wasmos/go/c)
foreach (_go IN ITEMS wasmos.go coroutine.go)
  file(COPY ${LIBC_DIR}/go/${_go} DESTINATION ${SDK_DIR}/share/wasmos/go)
endforeach ()
foreach (_c IN ITEMS coroutine_wasm.c ipc_future_wasm.c go_coroutine_trampoline.c
                     service_runtime_wasm.c go_async_app_wasm.c)
  file(COPY ${LIBSYS_WASM_DIR}/${_c} DESTINATION ${SDK_DIR}/share/wasmos/go/c)
endforeach ()

file(COPY ${SRC_DIR}/scripts/sdk/default-manifest.toml DESTINATION ${SDK_DIR}/share/wasmos)
file(COPY ${SRC_DIR}/scripts/sdk/WASMOSToolchain.cmake DESTINATION ${SDK_DIR}/share/cmake/WASMOS)
file(COPY ${SRC_DIR}/scripts/sdk/Platform/WASMOS.cmake
     DESTINATION ${SDK_DIR}/share/cmake/WASMOS/Platform)

# --- wasmos-sdk.conf -------------------------------------------------------
# Sourced by the wrappers. Tool paths are absolute because Stage 1 borrows the
# host LLVM rather than shipping its own; the sysroot itself is relocatable and is
# resolved from the wrapper's own path, never from here.
file(WRITE ${SDK_DIR}/wasmos-sdk.conf
"# Generated by cmake/wasmos_sdk_stage.cmake - do not edit.
WASMOS_SDK_VERSION=${SDK_VERSION}
WASMOS_SDK_TARGET=${SDK_TARGET}
WASMOS_SDK_TARGET_LLVM=${SDK_TARGET_LLVM}
WASMOS_SDK_CLANG=${SDK_CLANG}
WASMOS_SDK_CLANGXX=${SDK_CLANGXX}
WASMOS_SDK_ZIG=${SDK_ZIG}
WASMOS_SDK_ZIG_STACK_SIZE=${SDK_ZIG_STACK_SIZE}
WASMOS_SDK_ZIG_VA_LIMIT=${SDK_ZIG_VA_LIMIT}
WASMOS_SDK_ASC=${SDK_ASC}
WASMOS_SDK_RUSTC=${SDK_RUSTC}
WASMOS_SDK_TINYGO=${SDK_TINYGO}
WASMOS_SDK_WASMOPT=${SDK_WASMOPT}
")

file(WRITE ${STAMP} "staged\n")
