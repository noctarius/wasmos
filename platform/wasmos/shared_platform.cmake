# Minimal platform stubs for WASMOS (freestanding).

set(PLATFORM_SHARED_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions(-DBH_PLATFORM_WASMOS)
add_definitions(-DWASM_DISABLE_HW_BOUND_CHECK=1)
add_definitions(-DWASM_DISABLE_STACK_HW_BOUND_CHECK=1)
add_definitions(-DWASM_DISABLE_WAKEUP_BLOCKING_OP=1)
add_definitions(-DWASM_DISABLE_WRITE_GS_BASE=1)

include_directories(${PLATFORM_SHARED_DIR})

file(GLOB PLATFORM_SHARED_SOURCE
  ${PLATFORM_SHARED_DIR}/platform_init.c
  ${PLATFORM_SHARED_DIR}/platform_stub.c
)

set(PLATFORM_SHARED_SOURCE ${PLATFORM_SHARED_SOURCE})
