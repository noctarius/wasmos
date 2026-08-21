# WASMOSToolchain.cmake - CMake toolchain file for building WASMOS applications.
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=$WASMOS_SDK/share/cmake/WASMOS/WASMOSToolchain.cmake ..
#
# CMAKE_SYSTEM_NAME is WASMOS, which makes CMake load the platform module beside
# this file rather than guessing. It is deliberately not Linux: WASMOS satisfies
# none of the POSIX contracts a Linux platform module assumes.
set(CMAKE_SYSTEM_NAME WASMOS)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

get_filename_component(_wasmos_sdk_share "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(WASMOS_SDK_ROOT "${_wasmos_sdk_share}/.." ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

set(CMAKE_C_COMPILER   "${WASMOS_SDK_ROOT}/bin/wasmos-clang")
set(CMAKE_CXX_COMPILER "${WASMOS_SDK_ROOT}/bin/wasmos-clang++")
set(CMAKE_AR           "${WASMOS_SDK_ROOT}/bin/wasmos-ar")
set(CMAKE_RANLIB       "${WASMOS_SDK_ROOT}/bin/wasmos-ranlib")
set(CMAKE_SYSROOT      "${WASMOS_SDK_ROOT}/sysroot")

# The driver's output is a WASM module (and then a .wap), never a host
# executable, so CMake's default executable try-compile cannot run. Probe with a
# static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${WASMOS_SDK_ROOT}/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
