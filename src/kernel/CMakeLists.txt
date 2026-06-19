set(LDFLAGS_KERNEL -nostdlib -T ${KERNEL_DIR}/arch/x86_64/linker.ld)
if (WASMOS_WASM_RUNTIME_WARP)
  set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -mcmodel=kernel ${WASMOS_TRACE_DEFINE} ${WASMOS_RING3_SMOKE_DEFINE} ${WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFINE} ${WASMOS_PM_TEST_HOOKS_DEFINE} ${WASMOS_PM_LIST_DEFINE} ${WASMOS_PM_LIST_CAP_DEFINE} ${WASMOS_IRQ_MODE_DEFINE} ${WASMOS_SMP_DEFINE} ${WASMOS_SCHED_CALLER_CPU_BIAS_DEFINE} -DWASMOS_SCHED_THREADABLE -Wno-unused-parameter)
else ()
  set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -mcmodel=kernel ${WASMOS_TRACE_DEFINE} ${WASMOS_RING3_SMOKE_DEFINE} ${WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFINE} ${WASMOS_PM_TEST_HOOKS_DEFINE} ${WASMOS_PM_LIST_DEFINE} ${WASMOS_PM_LIST_CAP_DEFINE} ${WASMOS_IRQ_MODE_DEFINE} ${WASMOS_SMP_DEFINE} ${WASMOS_SCHED_CALLER_CPU_BIAS_DEFINE} -DWASMOS_SCHED_THREADABLE -I${WASM3_DIR}/source -Wno-unused-parameter)
endif ()
set(CXXFLAGS_KERNEL ${CFLAGS_KERNEL} -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wno-deprecated)
set(CFLAGS_WASM3 ${CFLAGS_KERNEL} -I${WASM3_DIR}/source -Wno-unused-parameter -Wno-unused-variable)

# WARP C flags (for posix_kernel.c and other WARP C glue — no C++ dialect, just compat includes).
set(CFLAGS_WARP_C ${CFLAGS_KERNEL} -I${KERNEL_DIR}/warp/compat)
# WARP C++14 flags: exceptions ON (WARP uses them internally), no RTTI, compat headers first.
execute_process(
  COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
  OUTPUT_VARIABLE _WARP_CLANG_RSRC
  OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CFLAGS_WARP_CXX ${CFLAGS_KERNEL}
    -std=c++14 -frtti -fexceptions -Wno-deprecated -Wno-unused-variable
    -DACTIVE_STACK_OVERFLOW_CHECK=1 -DLINEAR_MEMORY_BOUNDS_CHECKS=1
    -DVB_POSIX -DVB_WIN32_OR_POSIX -DNVALGRIND
    -I${KERNEL_DIR}/warp/compat
    -isystem ${_WARP_CLANG_RSRC}/include
    -Wno-macro-redefined -Wno-c++20-extensions -Wno-deprecated-copy
    "-Wno-#pragma-messages")
if (WASMOS_WARP_RING3)
  # ENABLE_ADVANCED_APIS=1 exposes getFunctionSignatureByName (needed to
  # locate JIT wrapper entry for IRET).  WASMOS_WARP_RING3 gates ring-3
  # execution code in the kernel wrapper layer.
  list(APPEND CFLAGS_WARP_CXX -DENABLE_ADVANCED_APIS=1 -DWASMOS_WARP_RING3)
  list(APPEND CFLAGS_WARP_C   -DWASMOS_WARP_RING3)
  # CFLAGS_KERNEL propagation is deferred to inside if(WASMOS_WASM_RUNTIME_WARP)
  # below so the wasm3-only build (WASMOS_WASM_RUNTIME_WARP=OFF) does not pick
  # up -DWASMOS_WARP_RING3 and reference WARP ring-3 symbols that are only
  # compiled as part of warp_driver.o / warp_link.o.
endif ()

set(RING3_NATIVE_PROBE_SRC ${KERNEL_DIR}/ring3_native_probe.c)
set(RING3_NATIVE_PROBE_OBJ ${BUILD_DIR}/ring3_native_probe.o)
set(RING3_NATIVE_PROBE_ELF ${BUILD_DIR}/ring3_native_probe.elf)
set(RING3_NATIVE_PROBE_BIN ${BUILD_DIR}/ring3_native_probe.bin)
set(RING3_NATIVE_PROBE_BLOB ${BUILD_DIR}/ring3_native_probe_blob.o)
set(RING3_THREADING_PROBE_SRC ${KERNEL_DIR}/ring3_thread_lifecycle_probe.c)
set(RING3_THREADING_PROBE_OBJ ${BUILD_DIR}/ring3_thread_lifecycle_probe.o)
set(RING3_THREADING_PROBE_ELF ${BUILD_DIR}/ring3_thread_lifecycle_probe.elf)
set(RING3_THREADING_PROBE_BIN ${BUILD_DIR}/ring3_thread_lifecycle_probe.bin)
set(RING3_THREADING_PROBE_BLOB ${BUILD_DIR}/ring3_thread_lifecycle_probe_blob.o)

add_custom_command(
  OUTPUT ${RING3_NATIVE_PROBE_BLOB}
  COMMAND ${CLANG} --target=x86_64-unknown-none-elf -Os -ffreestanding -fno-builtin
          -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
          -fomit-frame-pointer -mno-red-zone -nostdlib
          -I${LIBC_DIR}/include
          -c ${RING3_NATIVE_PROBE_SRC}
          -o ${RING3_NATIVE_PROBE_OBJ}
  COMMAND ${LLD_PATH} -nostdlib -T ${KERNEL_DIR}/arch/x86_64/ring3_native.ld
          -o ${RING3_NATIVE_PROBE_ELF}
          ${RING3_NATIVE_PROBE_OBJ}
  COMMAND ${OBJCOPY} --output-target=binary ${RING3_NATIVE_PROBE_ELF} ${RING3_NATIVE_PROBE_BIN}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          ring3_native_probe.bin ring3_native_probe_blob.o
  DEPENDS ${RING3_NATIVE_PROBE_SRC} ${KERNEL_DIR}/arch/x86_64/ring3_native.ld
          ${LIBC_DIR}/include/wasmos/syscall_x86_64.h
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Building native ring3 syscall probe blob"
  VERBATIM
)

add_custom_target(ring3_native_probe_blob DEPENDS ${RING3_NATIVE_PROBE_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${RING3_NATIVE_PROBE_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS ring3_native_probe_blob)

add_custom_command(
  OUTPUT ${RING3_THREADING_PROBE_BLOB}
  COMMAND ${CLANG} --target=x86_64-unknown-none-elf -Os -ffreestanding -fno-builtin
          -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
          -fomit-frame-pointer -mno-red-zone -nostdlib
          -I${LIBC_DIR}/include
          -c ${RING3_THREADING_PROBE_SRC}
          -o ${RING3_THREADING_PROBE_OBJ}
  COMMAND ${LLD_PATH} -nostdlib -T ${KERNEL_DIR}/arch/x86_64/ring3_native.ld
          -o ${RING3_THREADING_PROBE_ELF}
          ${RING3_THREADING_PROBE_OBJ}
  COMMAND ${OBJCOPY} --output-target=binary ${RING3_THREADING_PROBE_ELF} ${RING3_THREADING_PROBE_BIN}
  COMMAND ${OBJCOPY} --input-target=binary --output-target=elf64-x86-64
          --binary-architecture=i386:x86-64
          ring3_thread_lifecycle_probe.bin ring3_thread_lifecycle_probe_blob.o
  DEPENDS ${RING3_THREADING_PROBE_SRC} ${KERNEL_DIR}/arch/x86_64/ring3_native.ld
          ${LIBC_DIR}/include/wasmos/syscall_x86_64.h
  WORKING_DIRECTORY ${BUILD_DIR}
  COMMENT "Building ring3 thread lifecycle probe blob"
  VERBATIM
)

add_custom_target(ring3_thread_lifecycle_probe_blob DEPENDS ${RING3_THREADING_PROBE_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOBS ${RING3_THREADING_PROBE_BLOB})
set_property(GLOBAL APPEND PROPERTY WASMOS_KERNEL_BLOB_TARGETS ring3_thread_lifecycle_probe_blob)

set(KERNEL_OBJS
  ${BUILD_DIR}/entry.o
  ${BUILD_DIR}/context_switch.o
  ${BUILD_DIR}/cpu_isr.o
  ${BUILD_DIR}/smp_trampoline.o
  ${BUILD_DIR}/kernel.o
  ${BUILD_DIR}/kernel_boot_runtime.o
  ${BUILD_DIR}/kernel_init_runtime.o
  ${BUILD_DIR}/kernel_selftest_runtime.o
  ${BUILD_DIR}/kernel_sched_selftest_runtime.o
  ${BUILD_DIR}/kernel_ring3_fault_runtime.o
  ${BUILD_DIR}/kernel_ring3_probe_runtime.o
  ${BUILD_DIR}/kernel_ring3_smoke_runtime.o
  ${BUILD_DIR}/kernel_ring3_suite_runtime.o
  ${BUILD_DIR}/kernel_threading_selftest_runtime.o
  ${BUILD_DIR}/cpu.o
  ${BUILD_DIR}/cpu_x86_64.o
  ${BUILD_DIR}/memory.o
  ${BUILD_DIR}/capability.o
  ${BUILD_DIR}/policy.o
  ${BUILD_DIR}/slab.o
  ${BUILD_DIR}/syscall.o
  ${BUILD_DIR}/libc.o
  ${BUILD_DIR}/klog.o
  ${BUILD_DIR}/list.o
  ${BUILD_DIR}/list_linked.o
  ${BUILD_DIR}/list_array_chunk.o
  ${BUILD_DIR}/paging.o
  ${BUILD_DIR}/physmem.o
  ${BUILD_DIR}/runtime_context.o
  ${BUILD_DIR}/ipc.o
  ${BUILD_DIR}/process.o
  ${BUILD_DIR}/thread.o
  ${BUILD_DIR}/sched_thread.o
  ${BUILD_DIR}/sched_event.o
  ${BUILD_DIR}/poll.o
  ${BUILD_DIR}/futex.o
  ${BUILD_DIR}/spinlock.o
  ${BUILD_DIR}/user_mutex.o
  ${BUILD_DIR}/memory_service.o
  ${BUILD_DIR}/irq.o
  ${BUILD_DIR}/irq_x86_64.o
  ${BUILD_DIR}/lapic.o
  ${BUILD_DIR}/ioapic.o
  ${BUILD_DIR}/smp.o
  ${BUILD_DIR}/timer.o
  ${BUILD_DIR}/process_manager.o
  ${BUILD_DIR}/process_manager_buffers.o
  ${BUILD_DIR}/process_manager_services.o
  ${BUILD_DIR}/process_manager_spawn.o
  ${BUILD_DIR}/wasmos_app.o
  ${BUILD_DIR}/wasmos_app_meta.o
  ${BUILD_DIR}/native_driver.o
  ${BUILD_DIR}/wasm_chardev.o
  ${BUILD_DIR}/serial.o
  ${BUILD_DIR}/framebuffer.o
)
if (WASMOS_WASM_RUNTIME_WARP)
  list(APPEND KERNEL_OBJS
    ${BUILD_DIR}/warp_driver.o
    ${BUILD_DIR}/warp_shim.o
    ${BUILD_DIR}/warp_link.o
    ${BUILD_DIR}/warp_cxx_abi.o
    ${BUILD_DIR}/warp_posix.o
    ${BUILD_DIR}/warp_mem_utils.o
    ${BUILD_DIR}/warp_os_api_checker.o
    ${BUILD_DIR}/warp_iostream_stubs.o
  )
  if (WASMOS_WARP_RING3)
    list(APPEND KERNEL_OBJS ${BUILD_DIR}/warp_ring3_trampolines.o)
    # Propagate ring-3 define to kernel C files (syscall.c, smp.h, wasm_driver.h).
    # Placed here so it only takes effect when WARP is actually the runtime:
    # the ring-3 dispatch symbols (warp_r3_memory_helper, warp_ring3_dispatch)
    # live in warp_driver.o and warp_link.o which are only compiled above.
    set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -DWASMOS_WARP_RING3)
  endif ()
  list(APPEND KERNEL_OBJS
    ${BUILD_DIR}/warp_linker_stubs.o
    ${BUILD_DIR}/warp_sjlj_unwind.o
    ${BUILD_DIR}/warp_WasmModule.o
    ${BUILD_DIR}/warp_Runtime.o
    ${BUILD_DIR}/warp_ActiveMemoryManager.o
    ${BUILD_DIR}/warp_MemoryHelper.o
    ${BUILD_DIR}/warp_BinaryModule.o
    ${BUILD_DIR}/warp_VbExceptions.o
    ${BUILD_DIR}/warp_ExtendableMemory.o
    ${BUILD_DIR}/warp_VirtualMemoryAllocator.o
    ${BUILD_DIR}/warp_Compiler.o
    ${BUILD_DIR}/warp_BackendBase.o
    ${BUILD_DIR}/warp_x86_64_assembler.o
    ${BUILD_DIR}/warp_x86_64_backend.o
    ${BUILD_DIR}/warp_x86_64_call_dispatch.o
    ${BUILD_DIR}/warp_x86_64_cc.o
    ${BUILD_DIR}/warp_x86_64_instruction.o
    ${BUILD_DIR}/warp_x86_64_relpatchobj.o
    ${BUILD_DIR}/warp_Common.o
    ${BUILD_DIR}/warp_CommonRegsPos.o
    ${BUILD_DIR}/warp_MemWriter.o
    ${BUILD_DIR}/warp_ModuleInfo.o
    ${BUILD_DIR}/warp_Stack.o
    ${BUILD_DIR}/warp_BytecodeReader.o
    ${BUILD_DIR}/warp_Frontend.o
    ${BUILD_DIR}/warp_ValidationStack.o
  )
else ()
  list(APPEND KERNEL_OBJS
    ${BUILD_DIR}/wasm_driver.o
    ${BUILD_DIR}/wasm3_probe.o
    ${BUILD_DIR}/wasm3_link.o
    ${BUILD_DIR}/wasm3_shim.o
    ${BUILD_DIR}/wasm3_code.o
    ${BUILD_DIR}/wasm3_bind.o
    ${BUILD_DIR}/wasm3_compile.o
    ${BUILD_DIR}/wasm3_core.o
    ${BUILD_DIR}/wasm3_env.o
    ${BUILD_DIR}/wasm3_exec.o
    ${BUILD_DIR}/wasm3_function.o
    ${BUILD_DIR}/wasm3_info.o
    ${BUILD_DIR}/wasm3_module.o
    ${BUILD_DIR}/wasm3_parse.o
  )
endif ()

if (NOT DEFINED KERNEL_LINK_LIBS)
  set(KERNEL_LINK_LIBS "")
endif ()

set(KERNEL_SOURCES
  ${KERNEL_DIR}/arch/x86_64/entry.S
  ${KERNEL_DIR}/arch/x86_64/context_switch.S
  ${KERNEL_DIR}/arch/x86_64/cpu_isr.S
  ${KERNEL_DIR}/arch/x86_64/smp_trampoline.S
  ${KERNEL_DIR}/kernel.c
  ${KERNEL_DIR}/kernel_boot_runtime.c
  ${KERNEL_DIR}/kernel_init_runtime.c
  ${KERNEL_DIR}/kernel_selftest_runtime.c
  ${KERNEL_DIR}/kernel_sched_selftest_runtime.c
  ${KERNEL_DIR}/kernel_ring3_fault_runtime.c
  ${KERNEL_DIR}/kernel_ring3_probe_runtime.c
  ${KERNEL_DIR}/kernel_ring3_smoke_runtime.c
  ${KERNEL_DIR}/kernel_ring3_suite_runtime.c
  ${KERNEL_DIR}/kernel_threading_selftest_runtime.c
  ${KERNEL_DIR}/cpu.c
  ${KERNEL_DIR}/arch/x86_64/cpu_x86_64.c
  ${KERNEL_DIR}/libc.c
  ${KERNEL_DIR}/memory.c
  ${KERNEL_DIR}/capability.c
  ${KERNEL_DIR}/policy.c
  ${KERNEL_DIR}/slab.c
  ${KERNEL_DIR}/syscall.c
  ${KERNEL_DIR}/paging.c
  ${KERNEL_DIR}/klog.c
  ${KERNEL_DIR}/list.c
  ${KERNEL_DIR}/list_linked.c
  ${KERNEL_DIR}/list_array_chunk.c
  ${KERNEL_DIR}/physmem.c
  ${KERNEL_DIR}/runtime_context.c
  ${KERNEL_DIR}/ipc.c
  ${KERNEL_DIR}/process.c
  ${KERNEL_DIR}/thread.c
  ${KERNEL_DIR}/sched_thread.c
  ${KERNEL_DIR}/sched_event.c
  ${KERNEL_DIR}/poll.c
  ${KERNEL_DIR}/futex.c
  ${KERNEL_DIR}/spinlock.c
  ${KERNEL_DIR}/user_mutex.c
  ${KERNEL_DIR}/memory_service.c
  ${KERNEL_DIR}/irq.c
  ${KERNEL_DIR}/arch/x86_64/irq_x86_64.c
  ${KERNEL_DIR}/arch/x86_64/lapic.c
  ${KERNEL_DIR}/arch/x86_64/ioapic.c
  ${KERNEL_DIR}/arch/x86_64/smp.c
  ${KERNEL_DIR}/timer.c
  ${KERNEL_DIR}/process_manager.cpp
  ${KERNEL_DIR}/process_manager_buffers.c
  ${KERNEL_DIR}/process_manager_services.c
  ${KERNEL_DIR}/process_manager_spawn.c
  ${KERNEL_DIR}/wasmos_app.c
  ${KERNEL_DIR}/wasmos_app_meta.c
  ${KERNEL_DIR}/native_driver.c
  ${KERNEL_DIR}/wasm_chardev.c
  ${KERNEL_DIR}/serial.c
  ${KERNEL_DIR}/framebuffer.c
)
if (WASMOS_WASM_RUNTIME_WARP)
  list(APPEND KERNEL_SOURCES
    ${KERNEL_DIR}/warp_driver.cpp
    ${KERNEL_DIR}/warp/shim.cpp
    ${KERNEL_DIR}/warp/link.cpp
    ${KERNEL_DIR}/warp/cxx_abi.cpp
    ${KERNEL_DIR}/warp/posix_kernel.c
    ${KERNEL_DIR}/warp/mem_utils_kernel.cpp
  )
  if (WASMOS_WARP_RING3)
    list(APPEND KERNEL_SOURCES
      ${KERNEL_DIR}/warp/ring3_trampolines.c
    )
  endif ()
  list(APPEND KERNEL_SOURCES
    ${KERNEL_DIR}/warp/os_api_checker_kernel.cpp
    ${KERNEL_DIR}/warp/iostream_stubs.cpp
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
  )
else ()
  list(APPEND KERNEL_SOURCES
    ${KERNEL_DIR}/wasm_driver.c
    ${KERNEL_DIR}/wasm3/probe.c
    ${KERNEL_DIR}/wasm3/link.c
    ${KERNEL_DIR}/wasm3/shim.c
    ${WASM3_DIR}/source/m3_code.c
    ${WASM3_DIR}/source/m3_bind.c
    ${WASM3_DIR}/source/m3_compile.c
    ${WASM3_DIR}/source/m3_core.c
    ${WASM3_DIR}/source/m3_env.c
    ${WASM3_DIR}/source/m3_exec.c
    ${WASM3_DIR}/source/m3_function.c
    ${WASM3_DIR}/source/m3_info.c
    ${WASM3_DIR}/source/m3_module.c
    ${WASM3_DIR}/source/m3_parse.c
  )
endif ()

add_library(kernel_sources INTERFACE)
target_sources(kernel_sources INTERFACE
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/memory.h
  ${KERNEL_DIR}/include/capability.h
  ${KERNEL_DIR}/include/policy.h
  ${KERNEL_DIR}/include/slab.h
  ${KERNEL_DIR}/include/syscall.h
  ${KERNEL_DIR}/include/klog.h
  ${KERNEL_DIR}/include/list.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/runtime_context.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/thread.h
  ${KERNEL_DIR}/include/kernel_boot_runtime.h
  ${KERNEL_DIR}/include/kernel_init_runtime.h
  ${KERNEL_DIR}/include/kernel_selftest_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_fault_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_probe_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_smoke_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_suite_runtime.h
  ${KERNEL_DIR}/include/kernel_threading_selftest_runtime.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/process_manager_internal.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/timer.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/user_mutex.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/wasmos_app_meta.h
  ${KERNEL_DIR}/wasm3/link.h
  ${KERNEL_DIR}/wasm3/probe.h
  ${KERNEL_DIR}/wasm3/shim.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
)
target_include_directories(kernel_sources INTERFACE ${KERNEL_DIR} ${KERNEL_DIR}/include ${WASM3_DIR} ${DRIVER_WASM_DIR}/include)

# IDE aggregation target for CLion indexing (not part of the build).
add_library(kernel_ide OBJECT EXCLUDE_FROM_ALL
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/arch/x86_64/context_switch.S
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/memory.h
  ${KERNEL_DIR}/include/capability.h
  ${KERNEL_DIR}/include/policy.h
  ${KERNEL_DIR}/include/slab.h
  ${KERNEL_DIR}/include/syscall.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/runtime_context.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/thread.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/kernel_boot_runtime.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/timer.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/user_mutex.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
)
target_include_directories(kernel_ide PRIVATE
  ${KERNEL_DIR}
  ${KERNEL_DIR}/include
  ${LIBC_DIR}/include
  ${DRIVER_WASM_DIR}/include
  ${WASM3_DIR}
  ${WASM3_DIR}/source
)
target_compile_definitions(kernel_ide PRIVATE
  WASMOS_TRACE=${WASMOS_TRACE_VALUE}
  WASMOS_IRQ_MODE=${WASMOS_IRQ_MODE_VALUE})

get_property(WASMOS_KERNEL_BLOBS GLOBAL PROPERTY WASMOS_KERNEL_BLOBS)
if (WASMOS_KERNEL_BLOBS)
  list(APPEND KERNEL_OBJS ${WASMOS_KERNEL_BLOBS})
endif ()

# Build the runtime-specific compile command list that is spliced into the
# kernel.elf custom command below.  CMake expands lists within COMMAND args
# so COMMAND keywords inside the list become separate COMMAND clauses.
if (WASMOS_WASM_RUNTIME_WARP)
  set(WASM_RUNTIME_COMPILE_CMDS
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp_driver.cpp              -o ${BUILD_DIR}/warp_driver.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/shim.cpp                -o ${BUILD_DIR}/warp_shim.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/link.cpp                -o ${BUILD_DIR}/warp_link.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/cxx_abi.cpp             -o ${BUILD_DIR}/warp_cxx_abi.o
    COMMAND ${CLANG}              ${CFLAGS_WARP_C}    -c ${KERNEL_DIR}/warp/posix_kernel.c          -o ${BUILD_DIR}/warp_posix.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/mem_utils_kernel.cpp    -o ${BUILD_DIR}/warp_mem_utils.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/os_api_checker_kernel.cpp -o ${BUILD_DIR}/warp_os_api_checker.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/iostream_stubs.cpp      -o ${BUILD_DIR}/warp_iostream_stubs.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -frtti -c ${KERNEL_DIR}/warp/linker_stubs.cpp  -o ${BUILD_DIR}/warp_linker_stubs.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${KERNEL_DIR}/warp/sjlj_unwind.cpp           -o ${BUILD_DIR}/warp_sjlj_unwind.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/WasmModule/WasmModule.cpp  -o ${BUILD_DIR}/warp_WasmModule.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/runtime/Runtime.cpp   -o ${BUILD_DIR}/warp_Runtime.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/runtime/ActiveMemoryManager.cpp -o ${BUILD_DIR}/warp_ActiveMemoryManager.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/runtime/MemoryHelper.cpp -o ${BUILD_DIR}/warp_MemoryHelper.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/common/BinaryModule.cpp  -o ${BUILD_DIR}/warp_BinaryModule.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/common/VbExceptions.cpp  -o ${BUILD_DIR}/warp_VbExceptions.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/common/ExtendableMemory.cpp -o ${BUILD_DIR}/warp_ExtendableMemory.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/utils/VirtualMemoryAllocator.cpp -o ${BUILD_DIR}/warp_VirtualMemoryAllocator.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/Compiler.cpp            -o ${BUILD_DIR}/warp_Compiler.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/BackendBase.cpp -o ${BUILD_DIR}/warp_BackendBase.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_assembler.cpp    -o ${BUILD_DIR}/warp_x86_64_assembler.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_backend.cpp      -o ${BUILD_DIR}/warp_x86_64_backend.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_call_dispatch.cpp -o ${BUILD_DIR}/warp_x86_64_call_dispatch.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_cc.cpp            -o ${BUILD_DIR}/warp_x86_64_cc.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_instruction.cpp   -o ${BUILD_DIR}/warp_x86_64_instruction.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/backend/x86_64/x86_64_relpatchobj.cpp   -o ${BUILD_DIR}/warp_x86_64_relpatchobj.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/common/Common.cpp       -o ${BUILD_DIR}/warp_Common.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/common/CommonRegsPos.cpp -o ${BUILD_DIR}/warp_CommonRegsPos.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/common/MemWriter.cpp    -o ${BUILD_DIR}/warp_MemWriter.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/common/ModuleInfo.cpp   -o ${BUILD_DIR}/warp_ModuleInfo.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/common/Stack.cpp        -o ${BUILD_DIR}/warp_Stack.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/frontend/BytecodeReader.cpp  -o ${BUILD_DIR}/warp_BytecodeReader.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/frontend/Frontend.cpp        -o ${BUILD_DIR}/warp_Frontend.o
    COMMAND ${CMAKE_CXX_COMPILER} ${CFLAGS_WARP_CXX} -c ${WARP_DIR}/src/core/compiler/frontend/ValidationStack.cpp -o ${BUILD_DIR}/warp_ValidationStack.o
  )
  if (WASMOS_WARP_RING3)
    list(APPEND WASM_RUNTIME_COMPILE_CMDS
      COMMAND ${CLANG} ${CFLAGS_WARP_C} -c ${KERNEL_DIR}/warp/ring3_trampolines.c -o ${BUILD_DIR}/warp_ring3_trampolines.o
    )
  endif ()
else ()
  set(WASM_RUNTIME_COMPILE_CMDS
    COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_driver.c    -o ${BUILD_DIR}/wasm_driver.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${KERNEL_DIR}/wasm3/probe.c    -o ${BUILD_DIR}/wasm3_probe.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${KERNEL_DIR}/wasm3/link.c     -o ${BUILD_DIR}/wasm3_link.o
    COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm3/shim.c     -o ${BUILD_DIR}/wasm3_shim.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_code.c    -o ${BUILD_DIR}/wasm3_code.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_bind.c    -o ${BUILD_DIR}/wasm3_bind.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_compile.c -o ${BUILD_DIR}/wasm3_compile.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_core.c    -o ${BUILD_DIR}/wasm3_core.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_env.c     -o ${BUILD_DIR}/wasm3_env.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_exec.c    -o ${BUILD_DIR}/wasm3_exec.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_function.c -o ${BUILD_DIR}/wasm3_function.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_info.c    -o ${BUILD_DIR}/wasm3_info.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_module.c  -o ${BUILD_DIR}/wasm3_module.o
    COMMAND ${CLANG} ${CFLAGS_WASM3}  -c ${WASM3_DIR}/source/m3_parse.c   -o ${BUILD_DIR}/wasm3_parse.o
  )
endif ()

add_custom_command(
  OUTPUT ${BUILD_DIR}/kernel.elf
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/entry.S -o ${BUILD_DIR}/entry.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/context_switch.S -o ${BUILD_DIR}/context_switch.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/cpu_isr.S -o ${BUILD_DIR}/cpu_isr.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/smp_trampoline.S -o ${BUILD_DIR}/smp_trampoline.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel.c -o ${BUILD_DIR}/kernel.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_boot_runtime.c -o ${BUILD_DIR}/kernel_boot_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_init_runtime.c -o ${BUILD_DIR}/kernel_init_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_selftest_runtime.c -o ${BUILD_DIR}/kernel_selftest_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_sched_selftest_runtime.c -o ${BUILD_DIR}/kernel_sched_selftest_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_ring3_fault_runtime.c -o ${BUILD_DIR}/kernel_ring3_fault_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_ring3_probe_runtime.c -o ${BUILD_DIR}/kernel_ring3_probe_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_ring3_smoke_runtime.c -o ${BUILD_DIR}/kernel_ring3_smoke_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_ring3_suite_runtime.c -o ${BUILD_DIR}/kernel_ring3_suite_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel_threading_selftest_runtime.c -o ${BUILD_DIR}/kernel_threading_selftest_runtime.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/cpu.c -o ${BUILD_DIR}/cpu.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/cpu_x86_64.c -o ${BUILD_DIR}/cpu_x86_64.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/capability.c -o ${BUILD_DIR}/capability.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/policy.c -o ${BUILD_DIR}/policy.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/slab.c -o ${BUILD_DIR}/slab.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/syscall.c -o ${BUILD_DIR}/syscall.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/libc.c -o ${BUILD_DIR}/libc.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/klog.c -o ${BUILD_DIR}/klog.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/list.c -o ${BUILD_DIR}/list.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/list_linked.c -o ${BUILD_DIR}/list_linked.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/list_array_chunk.c -o ${BUILD_DIR}/list_array_chunk.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory.c -o ${BUILD_DIR}/memory.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/paging.c -o ${BUILD_DIR}/paging.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/physmem.c -o ${BUILD_DIR}/physmem.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/runtime_context.c -o ${BUILD_DIR}/runtime_context.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/ipc.c -o ${BUILD_DIR}/ipc.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process.c -o ${BUILD_DIR}/process.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/thread.c -o ${BUILD_DIR}/thread.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/sched_thread.c -o ${BUILD_DIR}/sched_thread.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/sched_event.c -o ${BUILD_DIR}/sched_event.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/poll.c -o ${BUILD_DIR}/poll.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/futex.c -o ${BUILD_DIR}/futex.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/spinlock.c -o ${BUILD_DIR}/spinlock.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/user_mutex.c -o ${BUILD_DIR}/user_mutex.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory_service.c -o ${BUILD_DIR}/memory_service.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/irq.c -o ${BUILD_DIR}/irq.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/irq_x86_64.c -o ${BUILD_DIR}/irq_x86_64.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/lapic.c -o ${BUILD_DIR}/lapic.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/ioapic.c -o ${BUILD_DIR}/ioapic.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/smp.c -o ${BUILD_DIR}/smp.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/timer.c -o ${BUILD_DIR}/timer.o
  COMMAND ${CMAKE_CXX_COMPILER} ${CXXFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager.cpp -o ${BUILD_DIR}/process_manager.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager_buffers.c -o ${BUILD_DIR}/process_manager_buffers.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager_services.c -o ${BUILD_DIR}/process_manager_services.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager_spawn.c -o ${BUILD_DIR}/process_manager_spawn.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasmos_app.c -o ${BUILD_DIR}/wasmos_app.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasmos_app_meta.c -o ${BUILD_DIR}/wasmos_app_meta.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/native_driver.c -o ${BUILD_DIR}/native_driver.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_chardev.c -o ${BUILD_DIR}/wasm_chardev.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/serial.c -o ${BUILD_DIR}/serial.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/framebuffer.c -o ${BUILD_DIR}/framebuffer.o
  ${WASM_RUNTIME_COMPILE_CMDS}
  COMMAND ${LLD_PATH} ${LDFLAGS_KERNEL} -o ${BUILD_DIR}/kernel.elf ${KERNEL_OBJS} ${KERNEL_LINK_LIBS}
  DEPENDS ${KERNEL_SOURCES} ${KERNEL_DIR}/arch/x86_64/smp_trampoline.S
  ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/kernel_init_runtime.h
  ${KERNEL_DIR}/include/kernel_boot_runtime.h
  ${KERNEL_DIR}/include/kernel_selftest_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_fault_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_probe_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_smoke_runtime.h
  ${KERNEL_DIR}/include/kernel_ring3_suite_runtime.h
  ${KERNEL_DIR}/include/kernel_threading_selftest_runtime.h
  ${KERNEL_DIR}/include/arch/x86_64/cpu_x86_64.h
  ${KERNEL_DIR}/include/ipc.h ${KERNEL_DIR}/include/memory.h ${KERNEL_DIR}/include/capability.h
  ${KERNEL_DIR}/include/policy.h
  ${KERNEL_DIR}/include/slab.h ${KERNEL_DIR}/include/syscall.h ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/klog.h
  ${KERNEL_DIR}/include/list.h
          ${KERNEL_DIR}/include/physmem.h ${KERNEL_DIR}/include/process.h ${KERNEL_DIR}/include/process_manager.h
          ${KERNEL_DIR}/include/process_manager_internal.h
          ${KERNEL_DIR}/include/thread.h
          ${KERNEL_DIR}/include/memory_service.h ${KERNEL_DIR}/include/irq.h ${KERNEL_DIR}/include/timer.h
          ${KERNEL_DIR}/include/arch/x86_64/irq_x86_64.h
          ${KERNEL_DIR}/include/arch/x86_64/lapic.h
          ${KERNEL_DIR}/include/arch/x86_64/ioapic.h
          ${KERNEL_DIR}/include/arch/x86_64/smp.h
          ${KERNEL_DIR}/include/serial.h ${KERNEL_DIR}/include/wasmos_app.h
          ${KERNEL_DIR}/include/wasmos_app_meta.h
          ${KERNEL_DIR}/include/native_driver.h ${KERNEL_DIR}/include/spinlock.h
          ${KERNEL_DIR}/include/user_mutex.h
          ${KERNEL_DIR}/include/wasm_driver.h ${KERNEL_DIR}/include/wasm_chardev.h
          ${KERNEL_DIR}/include/framebuffer.h
          ${KERNEL_DIR}/include/io.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building kernel"
  VERBATIM
)
add_custom_target(kernel ALL DEPENDS ${BUILD_DIR}/kernel.elf)
