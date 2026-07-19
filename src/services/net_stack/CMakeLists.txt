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
    -I${LIBSYS_NATIVE_DIR}/include)

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
          -e initialize
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

set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS ${WASM_NET_STACK_APP})
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS net_stack_app)
