# cmake/warp_aot_cached.cmake
# Invoked at build time via "cmake -P" to run warp_aot with content-addressed
# caching.  Skips AOT compilation when a cached result for the same WASM
# content + warp_aot binary already exists.
#
# Required variables (passed via -D on the cmake command line):
#   WARP_AOT    - path to the warp_aot host tool binary
#   WASM        - path to the input .wasm file
#   WARPBIN     - path to write the output .warpbin file
#   CACHE_DIR   - directory for cached .warpbin files (persists across clean builds)

cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED WARP_AOT OR NOT DEFINED WASM OR NOT DEFINED WARPBIN OR NOT DEFINED CACHE_DIR)
    message(FATAL_ERROR "warp_aot_cached.cmake: WARP_AOT, WASM, WARPBIN, and CACHE_DIR must be defined")
endif()

# Compute a combined content hash of the WASM input and the compiler binary.
# Including the compiler binary ensures cache invalidation when warp_aot is rebuilt.
file(SHA256 "${WASM}"     _wasm_hash)
file(SHA256 "${WARP_AOT}" _tool_hash)
string(SHA256 _cache_key "${_wasm_hash}${_tool_hash}")

set(_cached "${CACHE_DIR}/${_cache_key}.warpbin")

if (EXISTS "${_cached}")
    get_filename_component(_name "${WASM}" NAME)
    message(STATUS "warp_aot (cached): ${_name}")
    file(COPY "${_cached}" DESTINATION "${CACHE_DIR}")
    configure_file("${_cached}" "${WARPBIN}" COPYONLY)
else()
    execute_process(
        COMMAND "${WARP_AOT}" "${WASM}" "${WARPBIN}"
        RESULT_VARIABLE _rc
    )
    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "warp_aot failed (exit ${_rc}) for ${WASM}")
    endif()
    file(MAKE_DIRECTORY "${CACHE_DIR}")
    configure_file("${WARPBIN}" "${_cached}" COPYONLY)
endif()
