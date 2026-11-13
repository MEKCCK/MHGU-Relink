// Actual production item queue against synthetic guest memory and a deterministic clock.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"

static unsigned char image[0x1800000], heap[0x10000];
static void *test_guest(uint32_t address) {
    if (address < sizeof image) return image + address;
    assert(address >= 0x08000000u && address - 0x08000000u < sizeof heap);
    return heap + address - 0x08000000u;
}
#undef G2H
#define G2H(a) test_guest((uint32_t)(a))
#ifndef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER 0
static int pthread_mutex_lock(pthread_mutex_t *lock) { (void)lock; return 0; }
static int pthread_mutex_unlock(pthread_mutex_t *lock) { (void)lock; return 0; }
#endif
#include "../runtime/hle/itembar.c"

static const uint32_t player = 0x08001000u, vtable = 0x01000000u, select_fn = 0x123456u;
static uint64_t clock_ns;
static bool busy, allowed, local;
static unsigned turns, select_arg;
static uint32_t interrupt_fn;
static int interrupt_direction;

uint64_t plat_now_ns(void) { return clock_ns; }
uint32_t weapon_hunter(void) { return player; }
void mousecam_bow_charge_tick(Cpu *c) { (void)c; }
void mousecam_lock_shot(void) {}
void fnhook_call_original(Cpu *c, uint32_t fn) { (void)c; (void)fn; }
uint32_t hle_register(const char *name, HostFn fn) { (void)name; (void)fn; return HLE_BASE; }
bool fnhook_set(uint32_t fn, uint32_t detour) { (void)fn; (void)detour; return true; }
void guest_call(Cpu *c, uint32_t fn) {
    assert(c->r[0] == player);
    if (interrupt_fn == fn) {
        interrupt_fn = 0;
        clock_ns += 1000000;
        if (interrupt_direction) itembar_touch_scroll(interrupt_direction);
        else itembar_touch_cancel();
    }
    if (fn == FN_PL_IS_LOCAL) c->r[0] = local;
    else if (fn == FN_PL_BUSY) c->r[0] = busy;
    else if (fn == select_fn) {
        assert(c->r[1] == select_arg);
        c->r[0] = allowed;
    } else {
        assert(fn == FN_ITEM_NEXT || fn == FN_PROWLER_NEXT);
        unsigned slots = fn == FN_ITEM_NEXT ? 16 : 9;
        c->r[0] = (c->r[1] + (c->r[2] ? slots - 1 : 1)) % slots;
        ++turns;
    }
}

static void reset(void) {
    memset(image, 0, sizeof image); memset(heap, 0, sizeof heap);
    clock_ns = 1000000000ull;
    g_steps = g_touch_dir = 0;
    g_steps_ns = g_touch_ns = g_touch_serial = 0;
    g_seen_ns = clock_ns;
    busy = false; allowed = local = true; turns = 0; select_arg = 1;
    interrupt_fn = 0; interrupt_direction = 0;
    ST32(player, vtable); ST32(vtable + VF_CAN_SELECT, select_fn);
    ST16(player + PL_ITEM_SLOT, 5); ST16(player + OT_ITEM_SLOT, 5);
}
static void tick(const Bar *bar, uint64_t advance) {
    Cpu cpu = {};
    for (unsigned i = 0; i < 16; ++i) cpu.r[i] = 0xa0000000u + i;
    Cpu before = cpu;
    clock_ns += advance;
    turn(&cpu, player, bar);
    assert(memcmp(cpu.r, before.r, sizeof cpu.r) == 0);
}
static void replacement_and_release(void) {
    reset(); busy = true;
    for (unsigned i = 0; i < 100; ++i) itembar_touch_scroll(1);
    assert(g_touch_dir == 1 && !g_steps);
    itembar_touch_scroll(-1); assert(g_touch_dir == -1 && !g_steps);
    tick(&HUNTER, 16000000); assert(!turns && g_touch_dir == -1);
    busy = false;
    tick(&HUNTER, 16000000); assert(LD16(player + PL_ITEM_SLOT) == 4 && turns == 1 && !g_touch_dir);
    tick(&HUNTER, 16000000); assert(turns == 1);
    // Finger-up deliberately does not cancel: a fast accepted flick survives until the next guest update.
    itembar_touch_scroll(1);
    tick(&HUNTER, 33000000); assert(LD16(player + PL_ITEM_SLOT) == 5 && turns == 2);
    itembar_touch_scroll(1); itembar_touch_cancel(); // next finger-down discards the old gesture
    itembar_touch_scroll(-1);
    tick(&HUNTER, 33000000); assert(LD16(player + PL_ITEM_SLOT) == 4 && turns == 3);
    puts("PASS: production touch queue is a single replacement slot, reverses, and preserves a fast final finger-up step for the next guest tick");
}
static void wheel_isolation_and_expiry(void) {
    reset(); itembar_scroll(2); itembar_touch_scroll(-1); itembar_touch_cancel();
    assert(g_steps == 2 && !g_touch_dir);
    tick(&HUNTER, 33000000); tick(&HUNTER, 33000000);
    assert(turns == 2 && LD16(player + PL_ITEM_SLOT) == 7 && !g_steps);
    tick(&HUNTER, 33000000); assert(turns == 2);
    reset(); busy = true; itembar_scroll(1); itembar_touch_scroll(-1);
    tick(&HUNTER, TOUCH_KEEP_NS + 1);
    assert(!g_touch_dir && g_steps == 1 && !turns);
    busy = false; tick(&HUNTER, 33000000);
    assert(turns == 1 && LD16(player + PL_ITEM_SLOT) == 6 && !g_steps);
    reset(); busy = true; itembar_scroll(1);
    for (unsigned i = 0; i < 7; ++i) tick(&HUNTER, 100000000);
    assert(!g_steps && !turns); // the original wheel queue still has its own 600 ms busy timeout
    reset(); itembar_touch_scroll(1); itembar_scroll(1);
    tick(&HUNTER, ACTIVE_NS + 1); assert(!g_steps && !g_touch_dir && !turns);
    itembar_touch_scroll(1); g_seen_ns = clock_ns - ACTIVE_NS;
    itembar_touch_cancel(); itembar_touch_scroll(-1); assert(!g_touch_dir);
    puts("PASS: touch cancellation/120 ms expiry never consume wheel notches, wheel keeps its 600 ms timeout, and inactive handlers drop pending input");
}
static void in_flight_cancellation(void) {
    const uint32_t phases[] = {FN_PL_IS_LOCAL, FN_PL_BUSY, select_fn, FN_ITEM_NEXT};
    for (unsigned i = 0; i < sizeof phases / sizeof phases[0]; ++i) {
        reset(); itembar_touch_scroll(1); interrupt_fn = phases[i];
        tick(&HUNTER, 16000000);
        assert(LD16(player + PL_ITEM_SLOT) == 5 && !g_touch_dir);
        reset(); itembar_touch_scroll(1); interrupt_fn = phases[i]; interrupt_direction = -1;
        tick(&HUNTER, 16000000);
        assert(LD16(player + PL_ITEM_SLOT) == 5 && g_touch_dir == -1);
        tick(&HUNTER, 16000000);
        assert(LD16(player + PL_ITEM_SLOT) == 4 && !g_touch_dir);
    }
    reset(); itembar_scroll(1); itembar_touch_scroll(-1);
    interrupt_fn = FN_ITEM_NEXT;
    tick(&HUNTER, 16000000); assert(LD16(player + PL_ITEM_SLOT) == 5 && g_steps == 1);
    tick(&HUNTER, 16000000); assert(LD16(player + PL_ITEM_SLOT) == 6 && !g_steps);
    reset(); busy = true; itembar_touch_scroll(1);
    interrupt_fn = FN_PL_BUSY; interrupt_direction = -1;
    tick(&HUNTER, 16000000);
    assert(g_touch_dir == -1 && !turns); // a newer request is not expired using the older query's clock sample
    busy = false; tick(&HUNTER, 16000000);
    assert(LD16(player + PL_ITEM_SLOT) == 4 && !g_touch_dir);
    puts("PASS: cancellation or reversal during native permission/next-slot queries cannot commit a stale touch direction or swallow wheel input");
}
static void prowler_and_permissions(void) {
    reset(); select_arg = 0x10; busy = true;
    itembar_touch_scroll(-1); tick(&PROWLER, 33000000);
    assert(LD16(player + OT_ITEM_SLOT) == 4 && turns == 1); // Prowler has no hunter busy query
    reset(); allowed = false; itembar_touch_scroll(1);
    tick(&HUNTER, 33000000); assert(!turns && g_touch_dir == 1);
    tick(&HUNTER, TOUCH_KEEP_NS); allowed = true;
    tick(&HUNTER, 16000000); assert(!turns && !g_touch_dir);
    reset(); local = false; itembar_touch_scroll(1);
    tick(&HUNTER, 33000000); assert(!turns);
    puts("PASS: hunter/Prowler selection checks and caller registers remain intact");
}
int main(void) {
    replacement_and_release(); wheel_isolation_and_expiry();
    in_flight_cancellation(); prowler_and_permissions();
    puts("PASS: actual production touch-owned item queue (synthetic guest memory; no APK/game execution)");
    return 0;
}
