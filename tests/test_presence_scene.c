// Real scene classifier through loading/cutscene and confirmed village/title transitions.
#include <assert.h>
#include <string.h>
#include <hle.h>
typedef int SDL_AtomicInt;
static void SDL_SetAtomicInt(int *p,int value) { *p=value; }
static int SDL_GetAtomicInt(int *p) { return *p; }
static unsigned char globals[0x1b00000], objects[0x20000];
static void *memory(uint32_t address) {
    if(address<sizeof globals)return globals+address;
    assert(address>=0x08000000u && address<0x08000000u+sizeof objects);
    return objects+address-0x08000000u;
}
#undef G2H
#define G2H(a) memory(a)
#undef LOG
#define LOG(...) ((void)0)
static uint64_t now=1000000000ull;
uint64_t plat_now_ns(void) { return now; }
void harness_unlock_hook(pthread_mutex_t *m) { (void)m; }
#include <presence_under_test.h>
enum { GC=0x08001000, SP=0x08002000, HUNTER=0x08003000, PD=0x08006000, SQ=0x08007000, QD=0x08008000 };
static void tick(int seconds) { now+=(uint64_t)seconds*TICK_NS; presence_tick(); }
int main(void) {
    ST32(G_GAMECONTROL,GC); ST32(G_SPLAYER,SP); ST32(G_SQUEST,SQ);
    ST32(SP+0x1c,HUNTER); ST32(HUNTER+PL_DATA,PD); ST32(SQ+0xa4,QD);
    ST8(GC+GC_TOWN,1); tick(1); assert(!presence_in_quest());
    ST8(GC+GC_TOWN,TOWN_NONE); ST32(QD+8,42); tick(1); assert(presence_in_quest());
    ST32(SP+0x1c,0); tick(1); assert(presence_in_quest());
    tick(20); assert(presence_in_quest()); // CG/loading without a hunter must retain the quest profile.
    ST32(QD+8,0); tick(20); assert(presence_in_quest());
    ST32(SP+0x1c,HUNTER); tick(1); assert(presence_in_quest()); // Unknown in-between scene.
    ST8(GC+GC_TOWN,2); tick(1); assert(!presence_in_quest());
    ST8(GC+GC_TOWN,TOWN_NONE); ST32(QD+8,43); tick(1); assert(presence_in_quest());
    ST32(G_GAMECONTROL,0); ST32(SP+0x1c,0); tick(1); assert(presence_in_quest());
    tick(16); assert(!presence_in_quest()); // Stable title/menu can clear a stale quest state.
    puts("PASS: production scene classification keeps quest controls through missing-hunter/loading/CG states, exits in a confirmed village and expires at the title");
    return 0;
}
