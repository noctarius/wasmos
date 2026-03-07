#include "platform_internal.h"
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

// Minimal, non-functional stubs for freestanding builds.

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

int os_socket_send(bh_socket_t sock, const char *buf, int len) {
    (void)sock;
    (void)buf;
    (void)len;
    return -1;
}

int os_socket_recv(bh_socket_t sock, char *buf, int len) {
    (void)sock;
    (void)buf;
    (void)len;
    return -1;
}

int os_socket_close(bh_socket_t sock) {
    (void)sock;
    return 0;
}
