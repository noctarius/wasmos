# CMakeLists.txt for src/tools/warp_aot/
# Host-native WARP AOT compilation tool.
# Compiled with the standard host compiler — NOT freestanding.
# Only built when WARP is enabled (WASMOS_WASM_RUNTIME_WARP=ON).

set(WARP_DIR ${CMAKE_SOURCE_DIR}/libs/warp)

# WARP source files needed for host compilation (same set as the kernel,
# except mem_utils and linker stubs are replaced by host versions).
set(WARP_AOT_SRCS
    ${WARP_DIR}/src/WasmModule/WasmModule.cpp
    ${WARP_DIR}/src/core/runtime/Runtime.cpp
    ${WARP_DIR}/src/core/runtime/ActiveMemoryManager.cpp
    ${WARP_DIR}/src/core/runtime/MemoryHelper.cpp
    ${WARP_DIR}/src/core/common/BinaryModule.cpp
    ${WARP_DIR}/src/core/common/VbExceptions.cpp
    ${WARP_DIR}/src/core/common/ExtendableMemory.cpp
    ${WARP_DIR}/src/utils/VirtualMemoryAllocator.cpp
    ${WARP_DIR}/src/core/compiler/Compiler.cpp
    ${WARP_DIR}/src/core/compiler/backend/BackendBase.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_assembler.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_backend.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_call_dispatch.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_cc.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_instruction.cpp
    ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_relpatchobj.cpp
    ${WARP_DIR}/src/core/compiler/common/Common.cpp
    ${WARP_DIR}/src/core/compiler/common/CommonRegsPos.cpp
    ${WARP_DIR}/src/core/compiler/common/MemWriter.cpp
    ${WARP_DIR}/src/core/compiler/common/ModuleInfo.cpp
    ${WARP_DIR}/src/core/compiler/common/Stack.cpp
    ${WARP_DIR}/src/core/compiler/frontend/BytecodeReader.cpp
    ${WARP_DIR}/src/core/compiler/frontend/Frontend.cpp
    ${WARP_DIR}/src/core/compiler/frontend/ValidationStack.cpp
    # Host replacements (kernel versions use kernel allocator / physmem)
    host_mem_utils.cpp
    host_shim.cpp
    warp_aot.cpp
)

add_executable(warp_aot ${WARP_AOT_SRCS})

target_include_directories(warp_aot PRIVATE
    ${WARP_DIR}
    ${WARP_DIR}/src
)

target_compile_options(warp_aot PRIVATE
    -std=c++14
    -O2
    -DACTIVE_STACK_OVERFLOW_CHECK=1
    -DLINEAR_MEMORY_BOUNDS_CHECKS=1
    -DVB_POSIX
    -DVB_WIN32_OR_POSIX
    -DNVALGRIND
    -Wno-deprecated
    -Wno-unused-variable
    -Wno-macro-redefined
    -Wno-c++20-extensions
    -Wno-deprecated-copy
    -Wno-unused-parameter
)

set_target_properties(warp_aot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
