#ifndef WASMOS_PLATFORM_INTERNAL_H
#define WASMOS_PLATFORM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BH_PLATFORM_WASMOS
#define BH_PLATFORM_WASMOS
#endif

#define BH_APPLET_PRESERVED_STACK_SIZE (32 * 1024)
#define BH_THREAD_DEFAULT_PRIORITY 0

typedef uint32_t korp_tid;
typedef struct { int locked; } korp_mutex;
typedef struct { int signaled; } korp_cond;
typedef struct { int dummy; } korp_thread;
typedef struct { int dummy; } korp_rwlock;
typedef struct { int count; } korp_sem;

#define OS_THREAD_MUTEX_INITIALIZER {0}
#define os_thread_local_attribute __thread

#define bh_socket_t int

typedef int os_file_handle;
typedef void *os_dir_stream;
typedef int os_raw_file_handle;
typedef int os_poll_file_handle;
typedef uint32_t os_nfds_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} os_timespec;

static inline int os_getpagesize(void) {
    return 4096;
}

static inline os_file_handle os_get_invalid_handle(void) {
    return -1;
}

/* Minimal libc prototypes for freestanding build */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
long labs(long n);

#ifdef __cplusplus
}
#endif

#endif
