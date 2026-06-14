/* iostream_stubs.cpp - Definitions for the std::cout / std::cerr stubs declared
 * in compat/iostream.  Both are no-op ostream objects; no output is ever
 * produced since WARP's error print paths are dead in the kernel build. */

#include "compat/iostream"

namespace std {
    ostream cout;
    ostream cerr;
} // namespace std
