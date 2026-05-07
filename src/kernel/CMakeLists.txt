set(LDFLAGS_KERNEL -nostdlib -T ${KERNEL_DIR}/arch/x86_64/linker.ld)
set(CFLAGS_KERNEL ${CFLAGS_KERNEL} -mcmodel=kernel ${WASMOS_TRACE_DEFINE} ${WASMOS_RING3_SMOKE_DEFINE} ${WASMOS_LOW_SLOT_SWEEP_DEFINE} ${WASMOS_LOW_SLOT_SWEEP_LEVEL_DEFINE} ${WASMOS_IDENTITY_PD_COUNT_DEFINE} -I${WASM_DIR}/wasm3/source -Wno-unused-parameter)
set(CFLAGS_WASM3 ${CFLAGS_KERNEL} -I${WASM_DIR}/wasm3/source -Dd_m3HasFloat=0 -Wno-unused-parameter -Wno-unused-variable)

set(RING3_NATIVE_PROBE_SRC ${KERNEL_DIR}/ring3_native_probe.c)
set(RING3_NATIVE_PROBE_OBJ ${BUILD_DIR}/ring3_native_probe.o)
set(RING3_NATIVE_PROBE_ELF ${BUILD_DIR}/ring3_native_probe.elf)
set(RING3_NATIVE_PROBE_BIN ${BUILD_DIR}/ring3_native_probe.bin)
set(RING3_NATIVE_PROBE_BLOB ${BUILD_DIR}/ring3_native_probe_blob.o)

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

set(KERNEL_OBJS
  ${BUILD_DIR}/entry.o
  ${BUILD_DIR}/context_switch.o
  ${BUILD_DIR}/cpu_isr.o
  ${BUILD_DIR}/kernel.o
  ${BUILD_DIR}/cpu.o
  ${BUILD_DIR}/memory.o
  ${BUILD_DIR}/capability.o
  ${BUILD_DIR}/policy.o
  ${BUILD_DIR}/slab.o
  ${BUILD_DIR}/syscall.o
  ${BUILD_DIR}/libc.o
  ${BUILD_DIR}/paging.o
  ${BUILD_DIR}/physmem.o
  ${BUILD_DIR}/runtime_context.o
  ${BUILD_DIR}/ipc.o
  ${BUILD_DIR}/process.o
  ${BUILD_DIR}/spinlock.o
  ${BUILD_DIR}/memory_service.o
  ${BUILD_DIR}/irq.o
  ${BUILD_DIR}/timer.o
  ${BUILD_DIR}/process_manager.o
  ${BUILD_DIR}/wasmos_app.o
  ${BUILD_DIR}/native_driver.o
  ${BUILD_DIR}/wasm_driver.o
  ${BUILD_DIR}/wasm_chardev.o
  ${BUILD_DIR}/wasm3_probe.o
  ${BUILD_DIR}/wasm3_link.o
  ${BUILD_DIR}/wasm3_shim.o
  ${BUILD_DIR}/serial.o
  ${BUILD_DIR}/framebuffer.o
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

if (NOT DEFINED KERNEL_LINK_LIBS)
  set(KERNEL_LINK_LIBS "")
endif ()

set(KERNEL_SOURCES
  ${KERNEL_DIR}/arch/x86_64/entry.S
  ${KERNEL_DIR}/arch/x86_64/context_switch.S
  ${KERNEL_DIR}/arch/x86_64/cpu_isr.S
  ${KERNEL_DIR}/kernel.c
  ${KERNEL_DIR}/cpu.c
  ${KERNEL_DIR}/libc.c
  ${KERNEL_DIR}/memory.c
  ${KERNEL_DIR}/capability.c
  ${KERNEL_DIR}/policy.c
  ${KERNEL_DIR}/slab.c
  ${KERNEL_DIR}/syscall.c
  ${KERNEL_DIR}/paging.c
  ${KERNEL_DIR}/physmem.c
  ${KERNEL_DIR}/runtime_context.c
  ${KERNEL_DIR}/ipc.c
  ${KERNEL_DIR}/process.c
  ${KERNEL_DIR}/spinlock.c
  ${KERNEL_DIR}/memory_service.c
  ${KERNEL_DIR}/irq.c
  ${KERNEL_DIR}/timer.c
  ${KERNEL_DIR}/process_manager.c
  ${KERNEL_DIR}/wasmos_app.c
  ${KERNEL_DIR}/native_driver.c
  ${KERNEL_DIR}/wasm_driver.c
  ${KERNEL_DIR}/wasm_chardev.c
  ${KERNEL_DIR}/wasm3_probe.c
  ${KERNEL_DIR}/wasm3_link.c
  ${KERNEL_DIR}/wasm3_shim.c
  ${KERNEL_DIR}/serial.c
  ${KERNEL_DIR}/framebuffer.c
  ${WASM_DIR}/wasm3/source/m3_code.c
  ${WASM_DIR}/wasm3/source/m3_bind.c
  ${WASM_DIR}/wasm3/source/m3_compile.c
  ${WASM_DIR}/wasm3/source/m3_core.c
  ${WASM_DIR}/wasm3/source/m3_env.c
  ${WASM_DIR}/wasm3/source/m3_exec.c
  ${WASM_DIR}/wasm3/source/m3_function.c
  ${WASM_DIR}/wasm3/source/m3_info.c
  ${WASM_DIR}/wasm3/source/m3_module.c
  ${WASM_DIR}/wasm3/source/m3_parse.c
)

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
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/runtime_context.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/timer.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/wasm3_link.h
  ${KERNEL_DIR}/wasm3_probe.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
)
target_include_directories(kernel_sources INTERFACE ${KERNEL_DIR}/include ${WASM_DIR} ${DRIVER_WASM_DIR}/include)

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
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/timer.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  ${DRIVER_WASM_DIR}/chardev/chardev_server.c
  ${EXAMPLES_C_DIR}/chardev_client/chardev_client.c
  ${EXAMPLES_C_DIR}/hello/hello_c.c
  ${DRIVER_WASM_DIR}/ata/ata.c
  ${DRIVER_WASM_DIR}/fs_fat/fs_fat.c
  ${SERVICES_DIR}/sysinit/init.c
  ${SERVICES_DIR}/cli/cli.c
  ${SERVICES_DIR}/hw_discovery/hw_discovery.c
)
target_include_directories(kernel_ide PRIVATE
  ${KERNEL_DIR}/include
  ${LIBC_DIR}/include
  ${DRIVER_WASM_DIR}/include
  ${WASM_DIR}
  ${WASM_DIR}/wasm3/source
)
target_compile_definitions(kernel_ide PRIVATE WASMOS_TRACE=${WASMOS_TRACE_VALUE})

get_property(WASMOS_KERNEL_BLOBS GLOBAL PROPERTY WASMOS_KERNEL_BLOBS)
if (WASMOS_KERNEL_BLOBS)
  list(APPEND KERNEL_OBJS ${WASMOS_KERNEL_BLOBS})
endif ()

add_custom_command(
  OUTPUT ${BUILD_DIR}/kernel.elf
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/entry.S -o ${BUILD_DIR}/entry.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/context_switch.S -o ${BUILD_DIR}/context_switch.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/cpu_isr.S -o ${BUILD_DIR}/cpu_isr.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel.c -o ${BUILD_DIR}/kernel.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/cpu.c -o ${BUILD_DIR}/cpu.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/capability.c -o ${BUILD_DIR}/capability.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/policy.c -o ${BUILD_DIR}/policy.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/slab.c -o ${BUILD_DIR}/slab.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/syscall.c -o ${BUILD_DIR}/syscall.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/libc.c -o ${BUILD_DIR}/libc.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory.c -o ${BUILD_DIR}/memory.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/paging.c -o ${BUILD_DIR}/paging.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/physmem.c -o ${BUILD_DIR}/physmem.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/runtime_context.c -o ${BUILD_DIR}/runtime_context.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/ipc.c -o ${BUILD_DIR}/ipc.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process.c -o ${BUILD_DIR}/process.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/spinlock.c -o ${BUILD_DIR}/spinlock.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory_service.c -o ${BUILD_DIR}/memory_service.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/irq.c -o ${BUILD_DIR}/irq.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/timer.c -o ${BUILD_DIR}/timer.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager.c -o ${BUILD_DIR}/process_manager.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasmos_app.c -o ${BUILD_DIR}/wasmos_app.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/native_driver.c -o ${BUILD_DIR}/native_driver.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_driver.c -o ${BUILD_DIR}/wasm_driver.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_chardev.c -o ${BUILD_DIR}/wasm_chardev.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${KERNEL_DIR}/wasm3_probe.c -o ${BUILD_DIR}/wasm3_probe.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${KERNEL_DIR}/wasm3_link.c -o ${BUILD_DIR}/wasm3_link.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm3_shim.c -o ${BUILD_DIR}/wasm3_shim.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/serial.c -o ${BUILD_DIR}/serial.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/framebuffer.c -o ${BUILD_DIR}/framebuffer.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_code.c -o ${BUILD_DIR}/wasm3_code.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_bind.c -o ${BUILD_DIR}/wasm3_bind.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_compile.c -o ${BUILD_DIR}/wasm3_compile.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_core.c -o ${BUILD_DIR}/wasm3_core.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_env.c -o ${BUILD_DIR}/wasm3_env.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_exec.c -o ${BUILD_DIR}/wasm3_exec.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_function.c -o ${BUILD_DIR}/wasm3_function.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_info.c -o ${BUILD_DIR}/wasm3_info.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_module.c -o ${BUILD_DIR}/wasm3_module.o
  COMMAND ${CLANG} ${CFLAGS_WASM3} -c ${WASM_DIR}/wasm3/source/m3_parse.c -o ${BUILD_DIR}/wasm3_parse.o
  COMMAND ${LLD_PATH} ${LDFLAGS_KERNEL} -o ${BUILD_DIR}/kernel.elf ${KERNEL_OBJS} ${KERNEL_LINK_LIBS}
  DEPENDS ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h ${KERNEL_DIR}/include/memory.h ${KERNEL_DIR}/include/capability.h
  ${KERNEL_DIR}/include/policy.h
  ${KERNEL_DIR}/include/slab.h ${KERNEL_DIR}/include/syscall.h ${KERNEL_DIR}/include/paging.h
          ${KERNEL_DIR}/include/physmem.h ${KERNEL_DIR}/include/process.h ${KERNEL_DIR}/include/process_manager.h
          ${KERNEL_DIR}/include/memory_service.h ${KERNEL_DIR}/include/irq.h ${KERNEL_DIR}/include/timer.h
          ${KERNEL_DIR}/include/serial.h ${KERNEL_DIR}/include/wasmos_app.h
          ${KERNEL_DIR}/include/native_driver.h ${KERNEL_DIR}/include/spinlock.h
          ${KERNEL_DIR}/include/wasm_driver.h ${KERNEL_DIR}/include/wasm_chardev.h
          ${KERNEL_DIR}/include/framebuffer.h
          ${KERNEL_DIR}/include/io.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building kernel"
  VERBATIM
)
add_custom_target(kernel ALL DEPENDS ${BUILD_DIR}/kernel.elf)
