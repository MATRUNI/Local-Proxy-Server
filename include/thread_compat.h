#ifndef THREAD_COMPAT_H
#define THREAD_COMPAT_H

#if !defined(_WIN32)

#include <pthread.h>

#else

#include <windows.h>
#include <process.h>

// 1. Thread types
typedef HANDLE pthread_t;
typedef int    pthread_attr_t;

// 2. Thread attributes (Stack size handled or ignored)
#define pthread_attr_init(a)             ((void)0)
#define pthread_attr_setstacksize(a, s)  ((void)0)
#define pthread_attr_destroy(a)          ((void)0)

// 3. Mutexes (Native Windows SRW Locks - 0 overhead, static init)
typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER        SRWLOCK_INIT

// 4. Condition Variables (Native Windows Condition Variables)
typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_COND_INITIALIZER         CONDITION_VARIABLE_INIT

// 5. Thread Lifecycle
static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    (void)attr;
    *thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)start_routine, arg, 0, NULL);
    return (*thread == NULL) ? -1 : 0;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    (void)retval;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *c) {
    WakeConditionVariable(c);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}

#endif
#endif
