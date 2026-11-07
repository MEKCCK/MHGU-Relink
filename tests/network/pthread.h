#pragma once
// Harness-only subset: production Android builds use the platform pthreads.
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) { (void)attr; InitializeSRWLock(m); return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : (int)GetLastError(); }
static inline int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }
typedef struct { void *(*fn)(void *); void *arg; } HarnessThread;
static DWORD WINAPI harness_thread_start(LPVOID p) { HarnessThread args = *(HarnessThread *)p; free(p); args.fn(args.arg); return 0; }
static inline int pthread_create(pthread_t *t, const void *attrs, void *(*fn)(void *), void *arg) {
    (void)attrs; HarnessThread *a = malloc(sizeof *a); if (!a) return 12; a->fn = fn; a->arg = arg;
    *t = CreateThread(NULL, 0, harness_thread_start, a, 0, NULL); if (!*t) { free(a); return (int)GetLastError(); } return 0;
}
static inline int pthread_join(pthread_t t, void **result) { (void)result; DWORD r = WaitForSingleObject(t, INFINITE); CloseHandle(t); return r == WAIT_OBJECT_0 ? 0 : (int)r; }
static inline pthread_t pthread_self(void) { return GetCurrentThread(); }
static inline int pthread_detach(pthread_t t) { return CloseHandle(t) ? 0 : (int)GetLastError(); }
