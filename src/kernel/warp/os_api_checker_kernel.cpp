/* warp/os_api_checker_kernel.cpp - Kernel stub for WARP's OSAPIChecker.
 * checkSysCallReturn is called after WARP's POSIX calls land in the kernel stubs; it
 * reports a failure through klog instead of printing to std::cout. */

extern "C" {
#include "klog.h"
}

#include "src/utils/OSAPIChecker.hpp"

/* WARP's post-syscall check.  A non-zero `errorCode` is LOGGED and execution continues
 * — the failing operation is not retried and the caller is not told, so a WARP path
 * that relies on this aborting will run on with a failed syscall.  A null `msg` prints
 * "?".  Never panics, despite the file banner. */
void checkSysCallReturn(const char* const msg, int32_t const errorCode) {
    if (errorCode != 0) {
        klog_write("[warp] syscall failed: ");
        klog_write(msg ? msg : "?");
        klog_write("\n");
    }
}
