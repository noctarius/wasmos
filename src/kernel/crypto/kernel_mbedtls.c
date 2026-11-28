/* kernel_mbedtls.c - the platform functions kernel_mbedtls_config.h binds
 * mbedTLS to. Allocation goes to the kernel heap (kmem); the zeroing calloc
 * promises is done here. snprintf is reached only from string helpers the kernel
 * never calls, and writes nothing. */
#include <stdint.h>

#include "kmem.h"
#include "string.h"

#include "kernel_mbedtls_config.h"

void* kernel_mbedtls_calloc(size_t nmemb, size_t size) {
    if (nmemb != 0u && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    size_t bytes = nmemb * size;
    void* p = kmem_alloc(bytes ? bytes : 1u);
    if (p) {
        memset(p, 0, bytes);
    }
    return p;
}

void kernel_mbedtls_free(void* ptr) {
    if (ptr) {
        kmem_free(ptr);
    }
}

int kernel_mbedtls_snprintf(char* buf, size_t size, const char* fmt, ...) {
    (void)fmt;
    if (buf && size) {
        buf[0] = '\0';
    }
    return 0;
}
