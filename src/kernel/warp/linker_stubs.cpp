/* warp/linker_stubs.cpp - Linker stubs for symbols required when the WARP
 * backend is selected.
 *
 * Provides:
 *   - Minimal RTTI typeinfo vtables for __cxxabiv1 — the catch clauses in
 *     WARP's own sources need these emitted somewhere at link time.
 *   - malloc / memchr — standard library functions WARP or its dependencies
 *     call directly.
 *   - wasm3_* stubs — kernel_init_runtime.c and process.c reference wasm3
 *     symbols; in WARP mode they are never called but must link.
 *
 * The exception ABI itself (__cxa_*, __gxx_personality_v0, _Unwind_Resume)
 * lives in warp/cxx_abi.cpp. */

extern "C" {
#include "klog.h"
#include "slab.h"
}

// ---------------------------------------------------------------------------
// malloc / memchr — backed by kernel allocator / built-in
// (Exception unwinding stubs live in cxx_abi.cpp to avoid duplicates.)
// ---------------------------------------------------------------------------

extern "C" {

/* malloc for WARP's dependencies, backed by the kernel slab.  Bounded by the slab's
 * largest class, so a large request returns nullptr rather than falling back to pages;
 * the WARP module allocator (warp/shim.cpp) is the one that handles large blocks.  The
 * matching free() is in warp/posix_kernel.c. */
void* malloc(size_t size) {
    return kalloc_small(size);
}

/* memchr over exactly `n` bytes, comparing against the low 8 bits of `c`; returns a
 * mutable pointer into the const input (the caller must not write through it), or
 * nullptr when the byte does not occur.  Reads all of [s, s+n) — no NUL stops it. */
void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(s);
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == (unsigned char)c)
            return const_cast<unsigned char*>(&p[i]);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// wasm3 symbol stubs — kernel_init_runtime.c and process.c reference these;
// in WARP mode the probe path is never taken so they are unreachable.
// ---------------------------------------------------------------------------

struct boot_info;
/* wasm3 heap entry points referenced unconditionally by kernel_init_runtime.c and
 * process.c.  In a WARP build no wasm3 process exists, so these are never reached;
 * release does nothing and committed_bytes reports 0. */
void wasm3_heap_release(unsigned int) {}
unsigned long long wasm3_heap_committed_bytes(unsigned int) {
    return 0;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Minimal __cxxabiv1 RTTI typeinfo stubs — catch(...) and catch(std::exception&)
// require these typeinfo vtables to be present at link time.
// ---------------------------------------------------------------------------

namespace __cxxabiv1 {

/* Out-of-line virtual destructors force the vtable to be emitted here.
 * All virtual functions must be out-of-line for the vtable to be generated
 * in exactly one translation unit (the one holding the first non-inline
 * virtual function definition). */
/* The two typeinfo classes the Itanium C++ ABI names for a catch clause over a class
 * type.  Only their vtables matter: nothing here is ever consulted at runtime, because
 * warp/cxx_abi.cpp delivers exceptions by longjmp without type matching.  __base_type
 * is never read. */
struct __class_type_info {
    virtual ~__class_type_info();
};
__class_type_info::~__class_type_info() {}

struct __si_class_type_info : __class_type_info {
    const __class_type_info* __base_type;
    virtual ~__si_class_type_info();
};
__si_class_type_info::~__si_class_type_info() {}

} // namespace __cxxabiv1
