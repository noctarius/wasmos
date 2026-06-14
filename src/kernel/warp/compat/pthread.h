#pragma once
/* Minimal pthread stub — only the types are needed; none of the pthread
 * functions are called because getStackInfo() is never used in the kernel.
 * WARP includes <pthread.h> only inside #ifdef __linux__ or __APPLE__
 * paths, but the declaration appears in the outer POSIX scope. */
typedef unsigned long pthread_t;
typedef struct { unsigned long data[8]; } pthread_attr_t;
#ifdef __cplusplus
extern "C" {
#endif
pthread_t pthread_self(void);
int pthread_getattr_np(pthread_t, pthread_attr_t *);
int pthread_attr_getstack(const pthread_attr_t *, void **, __SIZE_TYPE__ *);
int pthread_attr_destroy(pthread_attr_t *);
void *pthread_get_stackaddr_np(pthread_t);
__SIZE_TYPE__ pthread_get_stacksize_np(pthread_t);
#ifdef __cplusplus
}
#endif
