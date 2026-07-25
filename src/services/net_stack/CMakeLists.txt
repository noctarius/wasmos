# net_stack service — native (non-WASM) lwIP net-stack packed as a .wap.
#
# Modeled on src/services/gfx_compositor/CMakeLists.txt: compile the service
# glue + lwIP core as freestanding objects, add the libsys_native exports object
# and the libc string/ctype objects (as the compositor does), link with
# `ld.lld -e initialize -static` into a native ELF, then pack with
# make_wasmos_app. Register into the global WASM app lists so it builds with the
# rest of the tree. The initfs packages this service and device-manager starts
# it from the boot rule; its PM and driver discovery is fully asynchronous.
#
# Socket state/ring attachment core is built and host-unit-tested. The live
# service registers `net.stack`, maps a client-provided descriptor and its
# persistent TX/RX grants, dispatches socket control requests, and binds the
# initial static virtio.net lwIP netif. UDP payload rings are live; TCP payload
# callbacks remain deferred.

set(NET_STACK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LWIP_DIR ${CMAKE_SOURCE_DIR}/libs/lwip)
set(MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/libs/mbedtls)

set(WASM_NET_STACK_ELF ${BUILD_DIR}/net_stack.elf)
set(WASM_NET_STACK_APP ${BUILD_DIR}/net_stack.wap)

# Freestanding compile flags matching the compositor's native objects.
set(NET_STACK_CFLAGS_BASE
    -target ${KERNEL_TARGET_TRIPLE}
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -fno-builtin
    -nostdlib
    -O2)

# Include tree: lwIP headers, our port dir (lwipopts.h + arch/cc.h), libc, and
# the native driver / libsys_native ABI headers.
set(NET_STACK_INCLUDES
    -I${LWIP_DIR}/src/include
    -I${NET_STACK_DIR}
    -I${LIBC_DIR}/include
    -I${DRIVER_WASM_DIR}/include
    -I${LIBSYS_WASM_DIR}/include
    -I${LIBSYS_NATIVE_DIR}/include
    # mbedTLS headers + the net-stack's slim freestanding config. net_stack.c and
    # the lwIP altcp_tls glue include mbedTLS; every TU that sees an mbedTLS
    # header MUST use the same MBEDTLS_CONFIG_FILE so struct layouts match. lwIP
    # core TUs get these too (harmless: they include no mbedTLS header).
    -I${MBEDTLS_DIR}/include
    -DMBEDTLS_CONFIG_FILE=\"net_stack_mbedtls_config.h\")

# lwIP is a never-edited git subtree; relax warnings-as-errors and silence the
# noisy-but-harmless diagnostics it emits under -Wall -Wextra.
set(NET_STACK_LWIP_WARN_FLAGS
    -Wno-error -Wno-unused-parameter -Wno-unused-variable
    -Wno-address -Wno-sign-compare)

# Minimal lwIP source set: COREFILES + CORE4FILES + the netif ethernet file,
# matching libs/lwip/src/Filelists.cmake (etharp lives under core/ipv4 in
# 2.2.x; ethernet.c under netif).
set(NET_STACK_LWIP_SOURCES
  ${LWIP_DIR}/src/core/init.c
  ${LWIP_DIR}/src/core/def.c
  ${LWIP_DIR}/src/core/dns.c
  ${LWIP_DIR}/src/core/inet_chksum.c
  ${LWIP_DIR}/src/core/ip.c
  ${LWIP_DIR}/src/core/mem.c
  ${LWIP_DIR}/src/core/memp.c
  ${LWIP_DIR}/src/core/netif.c
  ${LWIP_DIR}/src/core/pbuf.c
  ${LWIP_DIR}/src/core/raw.c
  ${LWIP_DIR}/src/core/stats.c
  ${LWIP_DIR}/src/core/sys.c
  ${LWIP_DIR}/src/core/altcp.c
  ${LWIP_DIR}/src/core/altcp_alloc.c
  ${LWIP_DIR}/src/core/altcp_tcp.c
  ${LWIP_DIR}/src/core/tcp.c
  ${LWIP_DIR}/src/core/tcp_in.c
  ${LWIP_DIR}/src/core/tcp_out.c
  ${LWIP_DIR}/src/core/timeouts.c
  ${LWIP_DIR}/src/core/udp.c
  ${LWIP_DIR}/src/core/ipv4/acd.c
  ${LWIP_DIR}/src/core/ipv4/autoip.c
  ${LWIP_DIR}/src/core/ipv4/dhcp.c
  ${LWIP_DIR}/src/core/ipv4/etharp.c
  ${LWIP_DIR}/src/core/ipv4/icmp.c
  ${LWIP_DIR}/src/core/ipv4/igmp.c
  ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
  ${LWIP_DIR}/src/core/ipv4/ip4.c
  ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
  ${LWIP_DIR}/src/netif/ethernet.c
)

set(NET_STACK_LWIP_HEADER_DEPS
  ${NET_STACK_DIR}/lwipopts.h
  ${NET_STACK_DIR}/arch/cc.h
)

# Per-source lwIP objects (flattened into BUILD_DIR with a net_stack_ prefix).
set(NET_STACK_OBJS)
foreach(_lwip_src ${NET_STACK_LWIP_SOURCES})
  get_filename_component(_lwip_name ${_lwip_src} NAME_WE)
  set(_lwip_obj ${BUILD_DIR}/net_stack_lwip_${_lwip_name}.o)
  list(APPEND NET_STACK_OBJS ${_lwip_obj})
  add_custom_command(
    OUTPUT ${_lwip_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
            ${NET_STACK_INCLUDES}
            -c ${_lwip_src}
            -o ${_lwip_obj}
    DEPENDS ${_lwip_src} ${NET_STACK_LWIP_HEADER_DEPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Compiling lwIP ${_lwip_name}.c (net-stack)"
    VERBATIM
  )
endforeach()

# --- mbedTLS (TLS backend for altcp_tls, milestone B) ----------------------
# mbedTLS is a never-edited git subtree. Compile every library/*.c the same way
# lwIP is built (freestanding, subtree warnings relaxed) against the slim
# freestanding config in net_stack_mbedtls_config.h. Modules the config leaves off compile
# to near-empty objects, so globbing the whole library is safe and self-pruning.
file(GLOB NET_STACK_MBEDTLS_SOURCES ${MBEDTLS_DIR}/library/*.c)
foreach(_mb_src ${NET_STACK_MBEDTLS_SOURCES})
  get_filename_component(_mb_name ${_mb_src} NAME_WE)
  set(_mb_obj ${BUILD_DIR}/net_stack_mbedtls_${_mb_name}.o)
  list(APPEND NET_STACK_OBJS ${_mb_obj})
  add_custom_command(
    OUTPUT ${_mb_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
            -I${NET_STACK_DIR}/freestanding
            -I${MBEDTLS_DIR}/include -I${NET_STACK_DIR} -I${LIBC_DIR}/include
            -DMBEDTLS_CONFIG_FILE=\"net_stack_mbedtls_config.h\"
            -c ${_mb_src}
            -o ${_mb_obj}
    DEPENDS ${_mb_src} ${NET_STACK_DIR}/net_stack_mbedtls_config.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Compiling mbedTLS ${_mb_name}.c (net-stack)"
    VERBATIM
  )
endforeach()

# lwIP's mbedTLS glue (altcp_tls_mbedtls.c + its mem helper). lwIP 2.2.1 targets
# the mbedTLS 2.x API; the 2.x/3.x gap is bridged WITHOUT editing libs/ by:
#   - shim headers under lwip_mbedtls_shim/mbedtls/ (certs.h, ssl_internal.h),
#     placed on an include path AHEAD of the real mbedTLS include, and
#   - net_stack_mbedtls3_compat.h force-included first (MBEDTLS_ALLOW_PRIVATE_ACCESS
#     + a mbedtls_pk_parse_key arity bridge).
set(NET_STACK_ALTCP_TLS_SOURCES
  ${LWIP_DIR}/src/apps/altcp_tls/altcp_tls_mbedtls.c
  ${LWIP_DIR}/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c)
set(NET_STACK_ALTCP_TLS_DEPS
  ${NET_STACK_LWIP_HEADER_DEPS}
  ${NET_STACK_DIR}/net_stack_mbedtls_config.h
  ${NET_STACK_DIR}/net_stack_mbedtls3_compat.h
  ${NET_STACK_DIR}/lwip_mbedtls_shim/mbedtls/certs.h
  ${NET_STACK_DIR}/lwip_mbedtls_shim/mbedtls/ssl_internal.h)
foreach(_tls_src ${NET_STACK_ALTCP_TLS_SOURCES})
  get_filename_component(_tls_name ${_tls_src} NAME_WE)
  set(_tls_obj ${BUILD_DIR}/net_stack_lwip_${_tls_name}.o)
  list(APPEND NET_STACK_OBJS ${_tls_obj})
  add_custom_command(
    OUTPUT ${_tls_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
            -I${NET_STACK_DIR}/lwip_mbedtls_shim
            -I${NET_STACK_DIR}/freestanding
            ${NET_STACK_INCLUDES}
            -include ${NET_STACK_DIR}/net_stack_mbedtls3_compat.h
            -c ${_tls_src}
            -o ${_tls_obj}
    DEPENDS ${_tls_src} ${NET_STACK_ALTCP_TLS_DEPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Compiling lwIP mbedTLS glue ${_tls_name}.c (net-stack)"
    VERBATIM
  )
endforeach()

# Service glue objects (our own code — build with warnings-as-errors relaxed
# only where lwIP headers force it; keep them lenient for parity with lwIP).
set(NET_STACK_NETSTACK_OBJ ${BUILD_DIR}/net_stack_net_stack.o)
add_custom_command(
  OUTPUT ${NET_STACK_NETSTACK_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
          ${NET_STACK_INCLUDES}
          -c ${NET_STACK_DIR}/net_stack.c
          -o ${NET_STACK_NETSTACK_OBJ}
  DEPENDS ${NET_STACK_DIR}/net_stack.c ${NET_STACK_LWIP_HEADER_DEPS}
          ${NET_STACK_DIR}/net_stack_mbedtls_config.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling net_stack.c (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_NETSTACK_OBJ})

set(NET_STACK_SOCKET_OBJ ${BUILD_DIR}/net_stack_socket.o)
add_custom_command(
  OUTPUT ${NET_STACK_SOCKET_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
          ${NET_STACK_INCLUDES}
          -c ${NET_STACK_DIR}/socket.c
          -o ${NET_STACK_SOCKET_OBJ}
  DEPENDS ${NET_STACK_DIR}/socket.c ${NET_STACK_DIR}/socket.h
          ${LIBSYS_WASM_DIR}/include/wasmos/ringbuf.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling socket.c (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_SOCKET_OBJ})

set(NET_STACK_IFCFG_OBJ ${BUILD_DIR}/net_stack_ifcfg.o)
add_custom_command(
  OUTPUT ${NET_STACK_IFCFG_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
          ${NET_STACK_INCLUDES}
          -c ${NET_STACK_DIR}/net_stack_ifcfg.c
          -o ${NET_STACK_IFCFG_OBJ}
  DEPENDS ${NET_STACK_DIR}/net_stack_ifcfg.c ${NET_STACK_DIR}/net_stack_ifcfg.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling net_stack_ifcfg.c (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_IFCFG_OBJ})

set(NET_STACK_PORT_OBJ ${BUILD_DIR}/net_stack_port.o)
add_custom_command(
  OUTPUT ${NET_STACK_PORT_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_LWIP_WARN_FLAGS}
          ${NET_STACK_INCLUDES}
          -c ${NET_STACK_DIR}/port.c
          -o ${NET_STACK_PORT_OBJ}
  DEPENDS ${NET_STACK_DIR}/port.c ${NET_STACK_LWIP_HEADER_DEPS}
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling port.c (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_PORT_OBJ})

# Single-worker native coroutine/future runtime. It is linked now so the
# native ring-3 service package continuously validates the x86_64 context
# switch ABI; net-stack adopts it for asynchronous socket operations next.
set(NET_STACK_COROUTINE_OBJ ${BUILD_DIR}/net_stack_coroutine_native.o)
add_custom_command(
  OUTPUT ${NET_STACK_COROUTINE_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_INCLUDES}
          -c ${LIBSYS_NATIVE_DIR}/coroutine_native.c
          -o ${NET_STACK_COROUTINE_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/coroutine_native.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/coroutine_native.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling native coroutine runtime (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_COROUTINE_OBJ})

set(NET_STACK_IPC_FUTURE_OBJ ${BUILD_DIR}/net_stack_ipc_future_native.o)
add_custom_command(
  OUTPUT ${NET_STACK_IPC_FUTURE_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_INCLUDES}
          -c ${LIBSYS_NATIVE_DIR}/ipc_future_native.c
          -o ${NET_STACK_IPC_FUTURE_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/ipc_future_native.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native.h
          ${LIBSYS_NATIVE_DIR}/include/wasmos/coroutine_native.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling native IPC future adapter (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_IPC_FUTURE_OBJ})

set(NET_STACK_SERVICE_RUNTIME_OBJ ${BUILD_DIR}/net_stack_service_runtime_native.o)
add_custom_command(
  OUTPUT ${NET_STACK_SERVICE_RUNTIME_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_INCLUDES}
          -c ${LIBSYS_NATIVE_DIR}/service_runtime_native.c
          -o ${NET_STACK_SERVICE_RUNTIME_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/service_runtime_native.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native.h
          ${LIBSYS_NATIVE_DIR}/include/wasmos/coroutine_native.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling native service coroutine runtime (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_SERVICE_RUNTIME_OBJ})

set(NET_STACK_ASYNC_ENTRY_OBJ ${BUILD_DIR}/net_stack_service_async_entry_native.o)
add_custom_command(
  OUTPUT ${NET_STACK_ASYNC_ENTRY_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE} ${NET_STACK_INCLUDES}
          -c ${LIBSYS_NATIVE_DIR}/service_async_entry_native.c
          -o ${NET_STACK_ASYNC_ENTRY_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/service_async_entry_native.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling native async service entry (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_ASYNC_ENTRY_OBJ})

set(NET_STACK_COROUTINE_SWITCH_OBJ ${BUILD_DIR}/net_stack_coroutine_native_x86_64.o)
add_custom_command(
  OUTPUT ${NET_STACK_COROUTINE_SWITCH_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} -target ${KERNEL_TARGET_TRIPLE}
          -c ${LIBSYS_NATIVE_DIR}/coroutine_native_x86_64.S
          -o ${NET_STACK_COROUTINE_SWITCH_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/coroutine_native_x86_64.S
          ${LIBSYS_NATIVE_DIR}/include/wasmos/coroutine_native.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Compiling native coroutine context switch (net-stack)"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_COROUTINE_SWITCH_OBJ})

# libsys_native exports object (same as gfx_compositor).
set(NET_STACK_LIBSYS_EXPORTS_OBJ ${BUILD_DIR}/net_stack_libsys_exports.o)
add_custom_command(
  OUTPUT ${NET_STACK_LIBSYS_EXPORTS_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE}
          -I${DRIVER_WASM_DIR}/include
          -I${LIBC_DIR}/include
          -I${LIBSYS_NATIVE_DIR}/include
          -c ${LIBSYS_NATIVE_DIR}/zig/libsys_native.c
          -o ${NET_STACK_LIBSYS_EXPORTS_OBJ}
  DEPENDS ${LIBSYS_NATIVE_DIR}/zig/libsys_native.c
          ${LIBSYS_NATIVE_DIR}/include/wasmos/libsys_native.h
          ${DRIVER_WASM_DIR}/include/wasmos_native_driver.h
          ${DRIVER_WASM_DIR}/include/wasmos_driver_abi.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building net-stack libsys exports object"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_LIBSYS_EXPORTS_OBJ})

# libc string + ctype objects (same as gfx_compositor). lwIP relies on
# mem*/str* helpers; ctype provides the range checks not superseded by
# LWIP_NO_CTYPE_H.
set(NET_STACK_LIBC_STRING_OBJ ${BUILD_DIR}/net_stack_libc_string.o)
add_custom_command(
  OUTPUT ${NET_STACK_LIBC_STRING_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE}
          -I${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/string.c
          -o ${NET_STACK_LIBC_STRING_OBJ}
  DEPENDS ${LIBC_DIR}/src/string.c ${LIBC_DIR}/include/string.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building net-stack libc string object"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_LIBC_STRING_OBJ})

set(NET_STACK_LIBC_CTYPE_OBJ ${BUILD_DIR}/net_stack_libc_ctype.o)
add_custom_command(
  OUTPUT ${NET_STACK_LIBC_CTYPE_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
  COMMAND ${CLANG} ${NET_STACK_CFLAGS_BASE}
          -I${LIBC_DIR}/include
          -c ${LIBC_DIR}/src/ctype.c
          -o ${NET_STACK_LIBC_CTYPE_OBJ}
  DEPENDS ${LIBC_DIR}/src/ctype.c ${LIBC_DIR}/include/ctype.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building net-stack libc ctype object"
  VERBATIM
)
list(APPEND NET_STACK_OBJS ${NET_STACK_LIBC_CTYPE_OBJ})

# Link the native ELF (entry = initialize, static).
add_custom_command(
  OUTPUT ${WASM_NET_STACK_ELF}
  COMMAND ${LLD_PATH}
          -e async_initialize
          -static
          ${NET_STACK_OBJS}
          -o ${WASM_NET_STACK_ELF}
  DEPENDS ${NET_STACK_OBJS}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Linking native net-stack service"
  VERBATIM
)

# Pack as a WASMOS-APP (.wap).
add_custom_command(
  OUTPUT ${WASM_NET_STACK_APP}
  COMMAND ${WASMOS_APP_PACKER}
          --manifest ${NET_STACK_DIR}/linker.metadata
          --in ${WASM_NET_STACK_ELF}
          --out ${WASM_NET_STACK_APP}
  DEPENDS ${WASM_NET_STACK_ELF} make_wasmos_app ${NET_STACK_DIR}/linker.metadata
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Packing native net-stack as WASMOS-APP"
  VERBATIM
)

add_custom_target(net_stack_app
  DEPENDS ${WASM_NET_STACK_APP}
  SOURCES ${NET_STACK_DIR}/net_stack.c ${NET_STACK_DIR}/socket.c ${NET_STACK_DIR}/socket.h
          ${NET_STACK_DIR}/port.c
          ${NET_STACK_DIR}/linker.metadata
)

# The packaged service is assembled through custom commands, which CMake does
# not export to compile_commands.json. Keep a matching object target solely
# for IDE navigation, diagnostics, and clangd/CLion indexing.
wasmos_add_ide_c_target(net_stack_ide
  SOURCES ${NET_STACK_DIR}/net_stack.c
          ${NET_STACK_DIR}/socket.c
          ${NET_STACK_DIR}/net_stack_ifcfg.c
          ${NET_STACK_DIR}/port.c
  INCLUDES ${LWIP_DIR}/src/include
           ${NET_STACK_DIR}
           ${LIBC_DIR}/include
           ${DRIVER_WASM_DIR}/include
           ${LIBSYS_WASM_DIR}/include
           ${LIBSYS_NATIVE_DIR}/include
           ${MBEDTLS_DIR}/include)
target_compile_options(net_stack_ide PRIVATE ${NET_STACK_CFLAGS_BASE}
    -DMBEDTLS_CONFIG_FILE=\"net_stack_mbedtls_config.h\")

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_NET_STACK_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS net_stack_app)
