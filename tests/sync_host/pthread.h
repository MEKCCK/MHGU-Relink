#pragma once
// Windows test adapter only. Android uses real pthreads and its FORTIFY checks.
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
typedef struct { SRWLOCK lock; int live; } pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER {SRWLOCK_INIT, 1}
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT
static int harness_destroyed_locks;
void harness_unlock_hook(pthread_mutex_t *m);
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *a) { (void)a; InitializeSRWLock(&m->lock); m->live=1; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { assert(m->live); m->live=0; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { if (!m->live) ++harness_destroyed_locks; AcquireSRWLockExclusive(&m->lock); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(&m->lock); harness_unlock_hook(m); return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const void *a) { (void)a; InitializeConditionVariable(c); return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return SleepConditionVariableSRW(c,&m->lock,INFINITE,0)?0:1; }
static inline uint64_t harness_realtime_ns(void) { FILETIME f; GetSystemTimePreciseAsFileTime(&f); ULARGE_INTEGER u; u.LowPart=f.dwLowDateTime; u.HighPart=f.dwHighDateTime; return (u.QuadPart-116444736000000000ull)*100; }
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts) {
    uint64_t end=(uint64_t)ts->tv_sec*1000000000ull+ts->tv_nsec, now=harness_realtime_ns();
    DWORD ms=end>now?(DWORD)((end-now+999999)/1000000):0;
    return SleepConditionVariableSRW(c,&m->lock,ms,0)?0:1;
}
static inline int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }
typedef struct { void *(*fn)(void *); void *arg; } HarnessThread;
static DWORD WINAPI harness_thread_start(LPVOID p) { HarnessThread a=*(HarnessThread *)p; free(p); a.fn(a.arg); return 0; }
static inline int pthread_create(pthread_t *t,const void *attr,void *(*fn)(void *),void *arg) {
    (void)attr; HarnessThread *a=malloc(sizeof *a); assert(a); a->fn=fn; a->arg=arg;
    *t=CreateThread(NULL,0,harness_thread_start,a,0,NULL); assert(*t); return 0;
}
static inline int pthread_join(pthread_t t,void **r) { (void)r; assert(WaitForSingleObject(t,10000)==WAIT_OBJECT_0); CloseHandle(t); return 0; }
