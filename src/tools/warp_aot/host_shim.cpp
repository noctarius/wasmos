/* host_shim.cpp - Minimal C++ ABI and allocator shim for warp_aot host tool.
 *
 * Provides:
 *   1. operator new/delete backed by malloc/free.
 *   2. RAIISignalHandler stub definitions.
 *      RAIISignalHandler.cpp has a compile-time bug when both
 *      ACTIVE_STACK_OVERFLOW_CHECK=1 and LINEAR_MEMORY_BOUNDS_CHECKS=1 are set
 *      (a 'struct sigaction sa' is declared inside a disabled #if block but
 *      then referenced in !ACTIVE_DIV_CHECK). This file provides the required
 *      static data and the two non-inline methods as no-ops instead. */

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <new>
#include <csignal>

#include "src/config.hpp"
#include "src/utils/RAIISignalHandler.hpp"

/* -----------------------------------------------------------------------
 * C++ memory operators
 * ----------------------------------------------------------------------- */

void *operator new(size_t size)
{
    void *p = malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void *operator new[](size_t size)
{
    void *p = malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void *p) noexcept   { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept   { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* nothrow variants */
void *operator new(size_t size, std::nothrow_t const &) noexcept   { return malloc(size); }
void *operator new[](size_t size, std::nothrow_t const &) noexcept { return malloc(size); }
void  operator delete(void *p, std::nothrow_t const &) noexcept   { free(p); }
void  operator delete[](void *p, std::nothrow_t const &) noexcept { free(p); }

/* -----------------------------------------------------------------------
 * RAIISignalHandler stub
 *
 * With ACTIVE_STACK_OVERFLOW_CHECK=1 and LINEAR_MEMORY_BOUNDS_CHECKS=1,
 * signal-based bounds/stack checking is not used.  All signal handler methods
 * are no-ops; we just need the static data members and the out-of-line methods.
 * ----------------------------------------------------------------------- */

namespace vb {

uint32_t RAIISignalHandler::runningCounter = 0;
std::mutex RAIISignalHandler::handlerMutex;
bool RAIISignalHandler::raiiSetSignalHandler = false;

#ifndef VB_WIN32
struct sigaction RAIISignalHandler::saSEGVOld   = {};
struct sigaction RAIISignalHandler::saSIGFPEOld = {};
#ifdef __APPLE__
struct sigaction RAIISignalHandler::saSIGBUSOld = {};
#endif
#endif

/* setSignalHandler: no-op; signal-based memory protection is disabled when
 * both bounds check flags are set. */
void RAIISignalHandler::setSignalHandler(
    SignalHandler const /* memorySignalHandler */,
    SignalHandler const /* divSignalHandler */)
{
    /* Both ACTIVE_STACK_OVERFLOW_CHECK and LINEAR_MEMORY_BOUNDS_CHECKS are
     * enabled, so WARP uses explicit bounds checks rather than signal handlers.
     * No signal handler registration is needed. */
}

void RAIISignalHandler::unsetSignalHandler() VB_NOEXCEPT
{
    /* No-op: see setSignalHandler above. */
}

void RAIISignalHandler::restoreSignalHandler() VB_NOEXCEPT
{
    /* No-op: see setSignalHandler above. */
}

} // namespace vb
