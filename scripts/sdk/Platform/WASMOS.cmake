# Platform/WASMOS.cmake - platform description for CMAKE_SYSTEM_NAME=WASMOS.
#
# WASMOS applications are WebAssembly modules packed into a .wap container. There
# are no shared libraries, no rpath, and no host-style executable to run, so the
# platform declares only what the generators need to stop guessing.
set(WASMOS 1)

set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_SUFFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_EXECUTABLE_SUFFIX ".wap")

set(CMAKE_DL_LIBS "")
set(CMAKE_SHARED_LIBRARY_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP "")
set(CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG "")

# WASMOS has no loader search path to seed, and no position-independent-code
# distinction: a WASM module is relocated by the runtime.
set(CMAKE_SYSTEM_INCLUDE_PATH /include)
set(CMAKE_SYSTEM_LIBRARY_PATH /lib)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
