// Exercises production synchronization functions, extracted verbatim by run_sync_host.py.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
static uint32_t guest[64];
#undef G2H
#define G2H(a) ((void *)((char *)guest+(a)))
#define CLOCK_REALTIME 0
static int clock_gettime(int clock,struct timespec *ts) { (void)clock; uint64_t n=harness_realtime_ns(); ts->tv_sec=n/1000000000; ts->tv_nsec=n%1000000000; return 0; }
uint64_t plat_now_ns(void) { return GetTickCount64()*1000000ull; }
void harness_unlock_hook(pthread_mutex_t *m) { (void)m; }
static HANDLE released, observed;
static DWORD waiter_id;
static int armed;
enum { MUTEX=16, KEY=20, WAITER=0x101, PRODUCER=0x102 };
static void store_hook(uint32_t *p,uint32_t value,int order) {
    __atomic_store_n(p,value,order);
    if (armed && p==&guest[MUTEX/4] && value==0 && GetCurrentThreadId()==waiter_id) {
        SetEvent(released); assert(WaitForSingleObject(observed,5000)==WAIT_OBJECT_0);
    }
}
#define __atomic_store_n(p,v,o) store_hook(p,v,o)
#include <kernel_under_test.h>
#undef __atomic_store_n
static uint32_t wait_result;
static uint32_t producer_key;
static KThread waiter;
static void *waiting(void *p) {
    (void)p; waiter_id=GetCurrentThreadId(); t_cur=&waiter;
    wait_result=svc_wait_process_wide_key(MUTEX,KEY,WAITER,20000000); return NULL;
}
static void *producing(void *p) {
    (void)p; assert(WaitForSingleObject(released,5000)==WAIT_OBJECT_0);
    uint32_t expected=0; assert(__atomic_compare_exchange_n(&guest[MUTEX/4],&expected,PRODUCER,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST));
    producer_key=__atomic_load_n(&guest[KEY/4],__ATOMIC_SEQ_CST); SetEvent(observed);
    if (producer_key) svc_signal_process_wide_key(KEY,1); // nn::os fast path skips the syscall if zero.
    svc_arbitrate_unlock(MUTEX); return NULL;
}
static void publication_race(void) {
    int rounds=REFERENCE_BUILD?25:1000;
    released=CreateEvent(NULL,TRUE,FALSE,NULL); observed=CreateEvent(NULL,TRUE,FALSE,NULL); assert(released&&observed);
    for (int i=0;i<rounds;i++) {
        memset(&waiter,0,sizeof waiter); pthread_cond_init(&waiter.cond,NULL);
        guest[MUTEX/4]=WAITER; guest[KEY/4]=0; ResetEvent(released); ResetEvent(observed); armed=1;
        pthread_t a,b; pthread_create(&b,NULL,producing,NULL); pthread_create(&a,NULL,waiting,NULL);
        pthread_join(a,NULL); pthread_join(b,NULL); armed=0;
        assert(g_waitlist==NULL);
        if (REFERENCE_BUILD) { assert(producer_key==0 && wait_result==R_TIMEOUT && guest[MUTEX/4]==0); }
        else { assert(producer_key==1 && wait_result==R_OK_ && guest[MUTEX/4]==WAITER); }
    }
    CloseHandle(released); CloseHandle(observed);
    printf("PASS: %s publication race, %d forced schedules (real production wait/signal/unlock)\n",REFERENCE_BUILD?"baseline reproduces lost notification":"fixed",rounds);
}
static void wait_semantics(void) {
    memset(&waiter,0,sizeof waiter); pthread_cond_init(&waiter.cond,NULL); t_cur=&waiter;
    guest[MUTEX/4]=WAITER; guest[KEY/4]=0;
    assert(svc_wait_process_wide_key(MUTEX,KEY,WAITER,0)==R_TIMEOUT); assert(!guest[MUTEX/4]);
    guest[MUTEX/4]=WAITER; waiter.cancel=true;
    assert(svc_wait_process_wide_key(MUTEX,KEY,WAITER,-1)==R_CANCELLED); assert(guest[MUTEX/4]==WAITER);
    KThread low={0},high={0}; pthread_cond_init(&low.cond,NULL); pthread_cond_init(&high.cond,NULL);
    low.prio=30; high.prio=10; low.wait_tag=0x110; high.wait_tag=0x111;
    low.wait_kind=high.wait_kind=WAIT_CONDVAR; low.wait_addr=high.wait_addr=KEY; low.wait_mutex=high.wait_mutex=MUTEX;
    low.wnext=&high; g_waitlist=&low; guest[MUTEX/4]=0; guest[KEY/4]=1;
    svc_signal_process_wide_key(KEY,1);
    assert(high.wait_kind==WAIT_NONE && low.wait_kind==WAIT_CONDVAR && guest[MUTEX/4]==high.wait_tag && guest[KEY/4]==1);
    svc_signal_process_wide_key(KEY,-1);
    assert(low.wait_kind==WAIT_MUTEX && !guest[KEY/4] && (guest[MUTEX/4]&WAITERS_BIT));
    svc_arbitrate_unlock(MUTEX);
    assert(low.wait_kind==WAIT_NONE && guest[MUTEX/4]==low.wait_tag);
    g_waitlist=NULL; t_cur=NULL; assert(!harness_destroyed_locks);
    puts("PASS: timeout releases mutex, cancellation reacquires it, priority signal and broadcast ownership handoff");
}
int main(void) { publication_race(); wait_semantics(); return 0; }
