// Production Decoder lifetime paths; FFmpeg releases and guest events are counted stubs.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <hle.h>
void rt_fatal_trace(void) { abort(); }
#undef LOG
#define LOG(...) ((void)0)
typedef void AVCodecContext;
typedef void AVFrame;
typedef void AVPacket;
static uint32_t guest_memory[512];
#undef G2H
#define G2H(a) ((void *)((char *)guest_memory+(a)))
static int resource_frees, decoder_frees, returned_buffers, signals;
static void release_resource(void **p) { assert(*p); *p=NULL; resource_frees++; }
#define avcodec_free_context release_resource
#define av_frame_free release_resource
#define av_packet_free release_resource
static void buf_put(uint32_t memory,uint32_t size) { assert(memory && size==64); returned_buffers++; }
static void guest_event_op(Cpu *c,uint32_t event,const char *fn) {
    (void)c; assert(event); if(strstr(fn,"SignalEvent")) signals++;
}
static void quarantine_free(void *p) { assert(p); decoder_frees++; }
static HANDLE paused,resume_worker;
static DWORD worker_id;
static pthread_mutex_t *decoder_lock;
static int armed;
void harness_unlock_hook(pthread_mutex_t *m) {
    if(armed && m==decoder_lock && GetCurrentThreadId()==worker_id) {
        armed=0; SetEvent(paused); assert(WaitForSingleObject(resume_worker,5000)==WAIT_OBJECT_0);
    }
}
#define free quarantine_free
#include <movie_under_test.h>
#undef free
#if REFERENCE_BUILD
#define dec_release_output_l dec_release_output
#endif
enum { OBJECT=128, SENTINEL=0x13579bdf };
static void *worker(void *p) {
    (void)p; worker_id=GetCurrentThreadId(); Cpu c={0}; c.r[0]=OBJECT; c.r[1]=0;
    dec_release_output_l(&c); assert(c.r[0]==0); return NULL;
}
static void schedule(void) {
    resource_frees=decoder_frees=returned_buffers=signals=harness_destroyed_locks=0;
    Decoder *d=calloc(1,sizeof *d); assert(d);
    pthread_mutex_init(&d->lock,NULL); decoder_lock=&d->lock;
#if !REFERENCE_BUILD
    d->refs=1;
#endif
    d->ctx=(void *)1; d->frame=(void *)2; d->pkt=(void *)3;
    d->npending=1; d->pending[0]=(void *)4;
    d->in_cap=d->out_cap=64; d->out_busy[0]=true;
    for(int i=0;i<NIN;i++)d->in_mem[i]=256+i*64;
    for(int i=0;i<NOUT;i++)d->out_mem[i]=512+i*64;
    for(int i=0;i<4;i++)d->events[i]=1024+i*4;
    ST32(4,SENTINEL); ST32(OBJECT+4,handle_new(d));
    ResetEvent(paused); ResetEvent(resume_worker); armed=1;
    pthread_t thread; pthread_create(&thread,NULL,worker,NULL);
    assert(WaitForSingleObject(paused,5000)==WAIT_OBJECT_0);
    Cpu c={0}; c.r[0]=OBJECT; dec_dtor(&c);
#if REFERENCE_BUILD
    assert(decoder_frees==1);
    assert(LD32(4)==0 && LD32(OBJECT+4)!=0); // RET(0) caused the old destructor to clear guest address 4.
#else
    assert(!decoder_frees && LD32(4)==SENTINEL && !LD32(OBJECT+4));
    assert(d->refs==1 && d->closing && d->stopped);
    c.r[0]=OBJECT; dec_start(&c,d); assert(c.r[0]==MOVIE_ERR);
    c.r[0]=OBJECT; dec_release_output_l(&c); assert(c.r[0]==MOVIE_ERR);
    c.r[0]=OBJECT; dec_dtor(&c); assert(!decoder_frees);
#endif
    int before=signals; SetEvent(resume_worker); pthread_join(thread,NULL);
    assert(signals==before && resource_frees==4 && decoder_frees==1 && returned_buffers==NIN+NOUT);
#if REFERENCE_BUILD
    assert(harness_destroyed_locks>0);
#else
    assert(!harness_destroyed_locks && !d->out_busy[0] && d->refs==0);
#endif
    free(d);
}
int main(void) {
    paused=CreateEvent(NULL,TRUE,FALSE,NULL); resume_worker=CreateEvent(NULL,TRUE,FALSE,NULL); assert(paused&&resume_worker);
    for(int i=0;i<100;i++)schedule();
    CloseHandle(paused); CloseHandle(resume_worker);
    printf("PASS: %s decoder release/destruction race (100 forced schedules), buffer/resource release and guest handle checks\n",REFERENCE_BUILD?"baseline reproduces destroyed-mutex access":"retained callbacks finish before destruction; late calls rejected");
    return 0;
}
