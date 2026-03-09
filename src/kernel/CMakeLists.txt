set(LDFLAGS_KERNEL -nostdlib -T ${KERNEL_DIR}/arch/x86_64/linker.ld)

set(KERNEL_OBJS
  ${BUILD_DIR}/entry.o
  ${BUILD_DIR}/cpu_isr.o
  ${BUILD_DIR}/kernel.o
  ${BUILD_DIR}/cpu.o
  ${BUILD_DIR}/memory.o
  ${BUILD_DIR}/paging.o
  ${BUILD_DIR}/physmem.o
  ${BUILD_DIR}/ipc.o
  ${BUILD_DIR}/process.o
  ${BUILD_DIR}/spinlock.o
  ${BUILD_DIR}/memory_service.o
  ${BUILD_DIR}/irq.o
  ${BUILD_DIR}/timer.o
  ${BUILD_DIR}/process_manager.o
  ${BUILD_DIR}/wasmos_app.o
  ${BUILD_DIR}/wasm_driver.o
  ${BUILD_DIR}/wamr_context.o
  ${BUILD_DIR}/wasm_chardev.o
  ${BUILD_DIR}/serial.o
  ${BUILD_DIR}/wamr_runtime.o
)

if (NOT DEFINED KERNEL_LINK_LIBS)
  set(KERNEL_LINK_LIBS "")
endif ()

set(KERNEL_SOURCES
  ${KERNEL_DIR}/arch/x86_64/entry.S
  ${KERNEL_DIR}/arch/x86_64/cpu_isr.S
  ${KERNEL_DIR}/kernel.c
  ${KERNEL_DIR}/cpu.c
  ${KERNEL_DIR}/memory.c
  ${KERNEL_DIR}/paging.c
  ${KERNEL_DIR}/physmem.c
  ${KERNEL_DIR}/ipc.c
  ${KERNEL_DIR}/process.c
  ${KERNEL_DIR}/spinlock.c
  ${KERNEL_DIR}/memory_service.c
  ${KERNEL_DIR}/irq.c
  ${KERNEL_DIR}/timer.c
  ${KERNEL_DIR}/process_manager.c
  ${KERNEL_DIR}/wasmos_app.c
  ${KERNEL_DIR}/wasm_driver.c
  ${KERNEL_DIR}/wamr_context.c
  ${KERNEL_DIR}/wasm_chardev.c
  ${KERNEL_DIR}/serial.c
  ${WASM_DIR}/wamr_runtime.c
)

add_library(kernel_sources INTERFACE)
target_sources(kernel_sources INTERFACE
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/memory.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/timer.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wamr_context.h
  ${KERNEL_DIR}/include/wasm_chardev.h
  ${KERNEL_DIR}/include/wasm_driver.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/io.h
  ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_init.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_stub.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_internal.h
  ${WASM_DIR}/wamr_runtime.h
)
target_include_directories(kernel_sources INTERFACE ${KERNEL_DIR}/include ${WASM_DIR} ${DRIVER_WASM_DIR}/include)

# IDE aggregation target for CLion indexing (not part of the build).
add_library(kernel_ide OBJECT EXCLUDE_FROM_ALL
  ${KERNEL_SOURCES}
  ${KERNEL_DIR}/include/boot.h
  ${KERNEL_DIR}/include/cpu.h
  ${KERNEL_DIR}/include/ipc.h
  ${KERNEL_DIR}/include/memory.h
  ${KERNEL_DIR}/include/paging.h
  ${KERNEL_DIR}/include/physmem.h
  ${KERNEL_DIR}/include/process.h
  ${KERNEL_DIR}/include/process_manager.h
  ${KERNEL_DIR}/include/memory_service.h
  ${KERNEL_DIR}/include/irq.h
  ${KERNEL_DIR}/include/timer.h
  ${KERNEL_DIR}/include/serial.h
  ${KERNEL_DIR}/include/wasmos_app.h
  ${KERNEL_DIR}/include/spinlock.h
  ${KERNEL_DIR}/include/wamr_context.h
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
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_init.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_stub.c
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos/platform_internal.h
  ${WASM_DIR}/wamr_runtime.h
)
target_include_directories(kernel_ide PRIVATE
  ${KERNEL_DIR}/include
  ${DRIVER_WASM_DIR}/include
  ${WAMR_DIR}/core/iwasm/include
  ${WASM_DIR}
  ${CMAKE_SOURCE_DIR}/src/wasm-micro-runtime/platform/wasmos
)

get_property(WASMOS_KERNEL_BLOBS GLOBAL PROPERTY WASMOS_KERNEL_BLOBS)
if (WASMOS_KERNEL_BLOBS)
  list(APPEND KERNEL_OBJS ${WASMOS_KERNEL_BLOBS})
endif ()

add_custom_command(
  OUTPUT ${BUILD_DIR}/kernel.elf
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/entry.S -o ${BUILD_DIR}/entry.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/arch/x86_64/cpu_isr.S -o ${BUILD_DIR}/cpu_isr.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/kernel.c -o ${BUILD_DIR}/kernel.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/cpu.c -o ${BUILD_DIR}/cpu.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory.c -o ${BUILD_DIR}/memory.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/paging.c -o ${BUILD_DIR}/paging.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/physmem.c -o ${BUILD_DIR}/physmem.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/ipc.c -o ${BUILD_DIR}/ipc.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process.c -o ${BUILD_DIR}/process.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/spinlock.c -o ${BUILD_DIR}/spinlock.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/memory_service.c -o ${BUILD_DIR}/memory_service.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/irq.c -o ${BUILD_DIR}/irq.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/timer.c -o ${BUILD_DIR}/timer.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/process_manager.c -o ${BUILD_DIR}/process_manager.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasmos_app.c -o ${BUILD_DIR}/wasmos_app.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_driver.c -o ${BUILD_DIR}/wasm_driver.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wamr_context.c -o ${BUILD_DIR}/wamr_context.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/wasm_chardev.c -o ${BUILD_DIR}/wasm_chardev.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${KERNEL_DIR}/serial.c -o ${BUILD_DIR}/serial.o
  COMMAND ${CLANG} ${CFLAGS_KERNEL} -c ${WASM_DIR}/wamr_runtime.c -o ${BUILD_DIR}/wamr_runtime.o
  COMMAND ${LLD_PATH} ${LDFLAGS_KERNEL} -o ${BUILD_DIR}/kernel.elf ${KERNEL_OBJS} ${KERNEL_LINK_LIBS}
  DEPENDS ${KERNEL_SOURCES} ${KERNEL_DIR}/include/boot.h ${KERNEL_DIR}/include/cpu.h
          ${KERNEL_DIR}/include/ipc.h ${KERNEL_DIR}/include/memory.h ${KERNEL_DIR}/include/paging.h
          ${KERNEL_DIR}/include/physmem.h ${KERNEL_DIR}/include/process.h ${KERNEL_DIR}/include/process_manager.h
          ${KERNEL_DIR}/include/memory_service.h ${KERNEL_DIR}/include/irq.h ${KERNEL_DIR}/include/timer.h
          ${KERNEL_DIR}/include/serial.h ${KERNEL_DIR}/include/wasmos_app.h ${KERNEL_DIR}/include/spinlock.h
          ${KERNEL_DIR}/include/wamr_context.h ${KERNEL_DIR}/include/wasm_driver.h ${KERNEL_DIR}/include/wasm_chardev.h
          ${KERNEL_DIR}/include/io.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h ${WASM_DIR}/wamr_runtime.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building kernel"
  VERBATIM
)
add_custom_target(kernel ALL DEPENDS ${BUILD_DIR}/kernel.elf)
