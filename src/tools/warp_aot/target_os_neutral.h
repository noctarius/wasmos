/* target_os_neutral.h - force-included into warp_aot's WARP code-generation
 * sources (src/tools/warp_aot/BUILD.py), so the code they generate does not
 * depend on the host the tool was built on.
 *
 * WARP's aarch64 backend chooses ABI details by the COMPILER's target OS:
 * __linux__ makes x18 allocatable (aarch64_cc.hpp, JUST_LINUX) and __APPLE__
 * packs stack-passed arguments of imported calls the Apple way
 * (aarch64_backend.cpp). The kernel builds WARP for aarch64-unknown-none-elf,
 * which defines neither, and an AOT binary must be what the kernel's own JIT
 * would produce, whichever host built it.
 *
 * The host's C++ library needs those macros, so every standard header WARP's
 * code-generation sources use is included here first, while they are still
 * defined; the macros are then removed, and the headers' include guards keep
 * them from being read again. A source that includes a header missing from
 * this list fails to compile rather than silently changing, because the
 * host's library refuses to build without them. */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <ostream>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#undef __APPLE__
#undef __linux__
