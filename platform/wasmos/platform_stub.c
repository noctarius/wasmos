#include "platform_internal.h"
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

// Minimal, non-functional stubs for freestanding builds.

static void byte_swap(uint8_t *a, uint8_t *b, size_t size) {
    while (size--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void quicksort(uint8_t *base, size_t nmemb, size_t size,
                      int (*compar)(const void *, const void *)) {
    if (nmemb < 2) {
        return;
    }
    size_t pivot = nmemb / 2;
    uint8_t *pivot_ptr = base + pivot * size;
    size_t i = 0;
    size_t j = nmemb - 1;
    while (i <= j) {
        while (compar(base + i * size, pivot_ptr) < 0) {
            i++;
        }
        while (compar(base + j * size, pivot_ptr) > 0) {
            if (j == 0) {
                break;
            }
            j--;
        }
        if (i <= j) {
            byte_swap(base + i * size, base + j * size, size);
            i++;
            if (j == 0) {
                break;
            }
            j--;
        }
    }
    if (j + 1 > 0) {
        quicksort(base, j + 1, size, compar);
    }
    quicksort(base + i * size, nmemb - i, size, compar);
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; ++i) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    if (!a || !b) {
        return (a == b) ? 0 : (a ? 1 : -1);
    }
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!delim || !saveptr) {
        return NULL;
    }
    char *s = str ? str : *saveptr;
    if (!s) {
        return NULL;
    }
    while (*s) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) {
            if (*s == *d) {
                is_delim = 1;
                break;
            }
            d++;
        }
        if (!is_delim) {
            break;
        }
        s++;
    }
    if (!*s) {
        *saveptr = NULL;
        return NULL;
    }
    char *token = s;
    while (*s) {
        const char *d = delim;
        while (*d) {
            if (*s == *d) {
                *s = '\0';
                *saveptr = s + 1;
                return token;
            }
            d++;
        }
        s++;
    }
    *saveptr = NULL;
    return token;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    (void)fmt;
    if (buf && n) {
        buf[0] = '\0';
    }
    return 0;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    (void)fmt;
    (void)ap;
    if (buf && n) {
        buf[0] = '\0';
    }
    return 0;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (!base || !compar || size == 0) {
        return;
    }
    quicksort((uint8_t *)base, nmemb, size, compar);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    if (!key || !base || !compar || size == 0) {
        return NULL;
    }
    size_t low = 0;
    size_t high = nmemb;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const uint8_t *elem = (const uint8_t *)base + mid * size;
        int cmp = compar(key, elem);
        if (cmp < 0) {
            high = mid;
        } else if (cmp > 0) {
            low = mid + 1;
        } else {
            return (void *)elem;
        }
    }
    return NULL;
}

long labs(long n) {
    return (n < 0) ? -n : n;
}

void abort(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void *wasm_runtime_malloc(unsigned int size) {
    return os_malloc(size);
}

void wasm_runtime_free(void *ptr) {
    os_free(ptr);
}

void *os_malloc(unsigned size) {
    (void)size;
    return NULL;
}

void *os_realloc(void *ptr, unsigned size) {
    (void)ptr;
    (void)size;
    return NULL;
}

void os_free(void *ptr) {
    (void)ptr;
}

int os_printf(const char *format, ...) {
    (void)format;
    return 0;
}

int os_vprintf(const char *format, va_list ap) {
    (void)format;
    (void)ap;
    return 0;
}

uint64 os_time_get_boot_us(void) {
    return 0;
}

uint64 os_time_thread_cputime_us(void) {
    return 0;
}

korp_tid os_self_thread(void) {
    return 0;
}

uint8 *os_thread_get_stack_boundary(void) {
    return NULL;
}

void os_thread_jit_write_protect_np(bool enabled) {
    (void)enabled;
}

int os_mutex_init(korp_mutex *mutex) {
    if (mutex) {
        mutex->locked = 0;
    }
    return 0;
}

int os_mutex_destroy(korp_mutex *mutex) {
    (void)mutex;
    return 0;
}

int os_mutex_lock(korp_mutex *mutex) {
    if (mutex) {
        mutex->locked = 1;
    }
    return 0;
}

int os_mutex_unlock(korp_mutex *mutex) {
    if (mutex) {
        mutex->locked = 0;
    }
    return 0;
}

void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file) {
    (void)hint;
    (void)size;
    (void)prot;
    (void)flags;
    (void)file;
    return NULL;
}

void os_munmap(void *addr, size_t size) {
    (void)addr;
    (void)size;
}

int os_mprotect(void *addr, size_t size, int prot) {
    (void)addr;
    (void)size;
    (void)prot;
    return 0;
}

void *os_mremap(void *old_addr, size_t old_size, size_t new_size) {
    (void)old_addr;
    (void)old_size;
    (void)new_size;
    return NULL;
}

void os_dcache_flush(void) {
}

void os_icache_flush(void *start, size_t len) {
    (void)start;
    (void)len;
}

int os_thread_create(korp_tid *p_tid, thread_start_routine_t start, void *arg,
                     unsigned int stack_size) {
    (void)p_tid;
    (void)start;
    (void)arg;
    (void)stack_size;
    return -1;
}

int os_thread_create_with_prio(korp_tid *p_tid, thread_start_routine_t start,
                               void *arg, unsigned int stack_size, int prio) {
    (void)p_tid;
    (void)start;
    (void)arg;
    (void)stack_size;
    (void)prio;
    return -1;
}

int os_thread_join(korp_tid thread, void **retval) {
    (void)thread;
    (void)retval;
    return -1;
}

int os_thread_detach(korp_tid thread) {
    (void)thread;
    return -1;
}

void os_thread_exit(void *retval) {
    (void)retval;
}

int os_thread_env_init(void) {
    return 0;
}

void os_thread_env_destroy(void) {
}

bool os_thread_env_inited(void) {
    return true;
}

int os_usleep(uint32 usec) {
    (void)usec;
    return 0;
}

int os_recursive_mutex_init(korp_mutex *mutex) {
    return os_mutex_init(mutex);
}

int os_cond_init(korp_cond *cond) {
    if (cond) {
        cond->signaled = 0;
    }
    return 0;
}

int os_cond_destroy(korp_cond *cond) {
    (void)cond;
    return 0;
}

int os_cond_signal(korp_cond *cond) {
    if (cond) {
        cond->signaled = 1;
    }
    return 0;
}

int os_cond_broadcast(korp_cond *cond) {
    return os_cond_signal(cond);
}

int os_cond_wait(korp_cond *cond, korp_mutex *mutex) {
    (void)cond;
    (void)mutex;
    return 0;
}

int os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds) {
    (void)cond;
    (void)mutex;
    (void)useconds;
    return 0;
}

int os_rwlock_init(korp_rwlock *lock) {
    (void)lock;
    return 0;
}

int os_rwlock_destroy(korp_rwlock *lock) {
    (void)lock;
    return 0;
}

int os_rwlock_rdlock(korp_rwlock *lock) {
    (void)lock;
    return 0;
}

int os_rwlock_wrlock(korp_rwlock *lock) {
    (void)lock;
    return 0;
}

int os_rwlock_unlock(korp_rwlock *lock) {
    (void)lock;
    return 0;
}

int os_sem_init(korp_sem *sem, int init_count) {
    if (sem) {
        sem->count = init_count;
    }
    return 0;
}

int os_sem_destroy(korp_sem *sem) {
    (void)sem;
    return 0;
}

int os_sem_wait(korp_sem *sem) {
    (void)sem;
    return 0;
}

int os_sem_post(korp_sem *sem) {
    (void)sem;
    return 0;
}

int os_sem_reltimedwait(korp_sem *sem, uint64 useconds) {
    (void)sem;
    (void)useconds;
    return 0;
}

int os_socket_init(bh_socket_t *sock, bool tcp, uint16 port) {
    (void)sock;
    (void)tcp;
    (void)port;
    return -1;
}

int os_socket_send(bh_socket_t socket, const void *buf, unsigned int len) {
    (void)socket;
    (void)buf;
    (void)len;
    return -1;
}

int os_socket_recv(bh_socket_t socket, void *buf, unsigned int len) {
    (void)socket;
    (void)buf;
    (void)len;
    return -1;
}

int os_socket_close(bh_socket_t sock) {
    (void)sock;
    return 0;
}
