#pragma once
/* Minimal pthread stub — types only.  The functions are declared but never
 * called: their sole WARP caller is MemUtils::getStackInfo(), and the kernel's
 * replacement (warp/mem_utils_kernel.cpp) returns a zeroed StackInfo instead. */
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
