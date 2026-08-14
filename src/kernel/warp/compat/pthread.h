#pragma once
/* compat/pthread.h — freestanding stub of <pthread.h>.
 *
 * Provides pthread_t and an opaque pthread_attr_t, plus declarations of
 * pthread_self, the Linux (pthread_getattr_np, pthread_attr_getstack,
 * pthread_attr_destroy) and macOS (pthread_get_stackaddr_np,
 * pthread_get_stacksize_np) stack-query calls.  Nothing else: no
 * pthread_create, no mutexes or condition variables, no TLS keys.  The kernel
 * has threads, but they are not pthreads.
 *
 * EVERY FUNCTION HERE IS DECLARATION ONLY — no definition exists anywhere in
 * the tree, so a call is an undefined-symbol link error rather than something
 * that fails at runtime.  That is sound because none is ever called: their sole
 * WARP caller is MemUtils::getStackInfo() in utils/MemUtils.cpp, which the
 * kernel build excludes in favour of warp/mem_utils_kernel.cpp, and that
 * returns a zeroed StackInfo instead of querying a thread.
 *
 * No translation unit in the kernel build includes this header today; the
 * subset above is what the kernel links, not a set derived from live uses.  The
 * pthread_attr_t layout is invented (eight words of padding) and is never
 * inspected by anything. */
typedef unsigned long pthread_t;
typedef struct {
    unsigned long data[8];
} pthread_attr_t;
#ifdef __cplusplus
extern "C" {
#endif
pthread_t pthread_self(void);
int pthread_getattr_np(pthread_t, pthread_attr_t*);
int pthread_attr_getstack(const pthread_attr_t*, void**, __SIZE_TYPE__*);
int pthread_attr_destroy(pthread_attr_t*);
void* pthread_get_stackaddr_np(pthread_t);
__SIZE_TYPE__ pthread_get_stacksize_np(pthread_t);
#ifdef __cplusplus
}
#endif
