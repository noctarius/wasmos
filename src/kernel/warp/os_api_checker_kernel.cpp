/* warp/os_api_checker_kernel.cpp - Kernel stub for WARP's OSAPIChecker.
 * checkSysCallReturn is called after kernel POSIX stubs; panics on error
 * instead of printing to std::cout. */

extern "C" {
#include "klog.h"
}

#include "src/utils/OSAPIChecker.hpp"

void checkSysCallReturn(const char *const msg, int32_t const errorCode)
{
    if (errorCode != 0) {
        klog_write("[warp] syscall failed: ");
        klog_write(msg ? msg : "?");
        klog_write("\n");
    }
}
