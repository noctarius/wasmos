/* iostream_stubs.cpp - Definitions for the std::cout / std::cerr stubs declared
 * in compat/iostream.  Both are no-op ostream objects; no output is ever
 * produced since WARP's error print paths are dead in the kernel build. */

#include "compat/iostream"

/* The two stream objects compat/ostream declares.  Both are ordinary global instances
 * of the no-op ostream, so every `<<` on them is discarded and no output reaches the
 * kernel log; construction order relative to other globals does not matter because they
 * hold no state.  Defined here rather than in the header so exactly one definition
 * exists across the WARP translation units. */
namespace std {
ostream cout;
ostream cerr;
} // namespace std
