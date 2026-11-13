// Actual production UI hooks + hunter resolution, with a synthetic guest and native predicate fixture.
// No live game or save: the native fixture is from main build fb08f1d20fd1204fe7070672777f869b.
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"

static unsigned char image[0x2400000], heap[0x20000];
static void *test_guest(uint32_t address) {
    if (address < sizeof image) return image + address;
    assert(address >= 0x08000000u && address - 0x08000000u < sizeof heap);
    return heap + address - 0x08000000u;
}
#undef G2H
#define G2H(a) test_guest((uint32_t)(a))
#include "../runtime/hle/weapon.c"
#include "../runtime/hle/bow_reticle.c"

static const uint32_t hunter = 0x08004000u, remote = 0x0800c000u, scope = 0x08010000u;
static HostFn registered[4], classifier_hook, aim_mode_hook;
static unsigned registrations, installs, fail_install, classifier_calls, aim_mode_calls, charge_calls;
static uint32_t class_result = UINT_MAX, gate_result = UINT_MAX;
static bool native_aim, clobber_next;
static Cpu next_input, next_output, *active_cpu;

bool mousecam_hunter_aiming(uint32_t h) { (void)h; return false; }

// The original function is a pure switch over these native actions, not input or a held timer.
static bool native_charge_fixture(uint32_t h) {
    unsigned st = LD16(h + PL_ACT_ST), no = LD16(h + PL_ACT_NO);
    if (st == 1) return no == 0x80 || no == 0x81 || no == 0x82;
    if (st == 4) return no == 0xa;
    if (st != 0x10) return false;
    switch (no) {
        case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: case 0xd:
        case 0x13: case 0x1b: case 0x2d: case 0x35: case 0x3b: case 0x3c: case 0x43: case 0x44:
            return true;
        default: return false;
    }
}

void guest_call(Cpu *c, uint32_t fn) {
    assert(fn == BOW_RT_CHARGING && c != active_cpu && c->r[0] == hunter);
    ++charge_calls;
    bool charging = native_charge_fixture(c->r[0]);
    // Simulate all the state a guest call is permitted to clobber, beyond the return value.
    memset(c, 0xa5, sizeof *c);
    c->r[0] = charging;
}

void fnhook_call_next(Cpu *c, uint32_t fn) {
    next_input = *c;
    uint32_t result;
    if (fn == BOW_RT_CLASSIFY) {
        ++classifier_calls;
        result = class_result != UINT_MAX ? class_result : native_aim && !LD8(c->r[1] + 0x2716) ? 2 : 8;
    } else {
        assert(fn == BOW_RT_AIM_MODE);
        ++aim_mode_calls;
        result = gate_result != UINT_MAX ? gate_result : LD8(c->r[0] + 0x2716) != 0;
    }
    if (clobber_next) memset(c, 0x6b, sizeof *c);
    c->r[0] = result;
    next_output = *c;
}

uint32_t hle_register(const char *name, HostFn fn) {
    assert(name && registrations < 4);
    registered[registrations] = fn;
    return HLE_BASE + 16 * registrations++;
}
bool fnhook_set_outer(uint32_t fn, uint32_t detour) {
    ++installs;
    assert(detour >= HLE_BASE && (detour - HLE_BASE) / 16 < registrations);
    HostFn hook = registered[(detour - HLE_BASE) / 16];
    if (fn == BOW_RT_CLASSIFY) { if (fail_install & 1) return false; classifier_hook = hook; }
    else { assert(fn == BOW_RT_AIM_MODE); if (fail_install & 2) return false; aim_mode_hook = hook; }
    return true;
}

static void action(unsigned state, unsigned number) {
    ST16(hunter + PL_ACT_ST, state); ST16(hunter + PL_ACT_NO, number);
}
static void world(void) {
    memset(heap, 0, sizeof heap);
    uint32_t gc = 0x08000000, sp = 0x08001000;
    ST32(G_GAMECONTROL, gc); ST32(G_SPLAYER, sp);
    ST32(gc + 0x30, 0); ST32(sp + 0x1c, hunter);
    ST32(hunter, VT_BOW); ST32(remote, VT_BOW);
    ST32(VT_HEAVY + 4 * VT_DRAWN, FN_DRAWN);
    ST32(VT_LIGHT + 4 * VT_DRAWN, FN_DRAWN); ST32(VT_BOW + 4 * VT_DRAWN, FN_DRAWN);
    ST8(hunter + PL_FLAGS, 1); action(1, 0);
    native_aim = clobber_next = false;
    class_result = gate_result = UINT_MAX;
    classifier_calls = aim_mode_calls = charge_calls = 0;
}
static void install(unsigned failure) {
    registrations = installs = 0; fail_install = failure;
    classifier_hook = aim_mode_hook = NULL;
    bow_reticle_init();
    assert(installs == 2 && registrations == 2);
    assert(g_bow_reticle_ready == (failure == 0));
}
static void invoke(Cpu *c, uint32_t fn) {
    active_cpu = c;
    HostFn hook = fn == BOW_RT_CLASSIFY ? classifier_hook : aim_mode_hook;
    if (hook) hook(c); else fnhook_call_next(c, fn);
    active_cpu = NULL;
}
static Cpu seeded(uint32_t r0, uint32_t r1, uint32_t lr) {
    Cpu cpu; memset(&cpu, 0x38, sizeof cpu);
    cpu.r[0] = r0; cpu.r[1] = r1; cpu.r[14] = lr;
    return cpu;
}
static void assert_only_result(const Cpu *c, uint32_t result) {
    Cpu expected = next_output; expected.r[0] = result;
    assert(memcmp(c, &expected, sizeof expected) == 0);
}

// Model only the already-proved native category/visibility branch; drawing remains in the game.
static bool screen_reticle(bool suppressed) {
    Cpu cpu = seeded(scope, hunter, BOW_RT_CLASSIFY_LR);
    invoke(&cpu, BOW_RT_CLASSIFY);
    if (cpu.r[0] == 2) return true;  // native manual-aim path can otherwise bypass the gate
    assert(cpu.r[0] == 8);
    if (suppressed) return false;   // preserve the game's pause/suppression check before the gate
    cpu = seeded(hunter, 0, BOW_RT_AIM_MODE_LR);
    invoke(&cpu, BOW_RT_AIM_MODE);
    return cpu.r[0] != 0;
}

static void charging_contract(void) {
    const unsigned states[][2] = {{1,0x80},{1,0x81},{1,0x82},{4,0xa},
        {0x10,2},{0x10,3},{0x10,4},{0x10,5},{0x10,6},{0x10,7},{0x10,0xd},{0x10,0x13},
        {0x10,0x1b},{0x10,0x2d},{0x10,0x35},{0x10,0x3b},{0x10,0x3c},{0x10,0x43},{0x10,0x44}};
    const unsigned ends[][2] = {{1,0},{1,0x55},{2,0},{0x10,0},{0x10,1},{0x10,8},
        {0x10,0x1c},{0x10,0x2f},{0x10,0x33},{0x10,0x34},{0x10,0x45},{0x8010,0xd}};
    world(); install(0);
    for (int type = 0; type < 2; ++type) {
        ST8(LD32(G_GAMECONTROL) + 0x6c, type);
        for (unsigned manual = 0; manual < 2; ++manual) {
            native_aim = manual;
            for (unsigned flag = 0; flag < 2; ++flag) {
                ST8(hunter + 0x2716, flag);
                for (unsigned i = 0; i < sizeof states / sizeof *states; ++i) {
                    action(states[i][0], states[i][1]);
                    assert(screen_reticle(false));
                    assert(!screen_reticle(true));
                }
                for (unsigned i = 0; i < sizeof ends / sizeof *ends; ++i) {
                    action(0x10,0xd); assert(screen_reticle(false));
                    action(ends[i][0], ends[i][1]); assert(!screen_reticle(false));
                }
                action(0x10,0xd); ST8(hunter + PL_FLAGS, 0);
                unsigned before = charge_calls;
                assert(!screen_reticle(false) && charge_calls == before);
                ST8(hunter + PL_FLAGS, 1);
            }
        }
    }
    puts("PASS: all 19 native charging actions show the screen reticle for Bow Types 1/2; native aim/flag do not bypass release, shot, cancel or sheath hiding; native suppression remains.");
}

static void isolated_calls(void) {
    world(); install(0); action(0x10,0xd);
    class_result = 0x55; gate_result = 0x66; clobber_next = true;
    Cpu cpu = seeded(scope, hunter, BOW_RT_CLASSIFY_LR);
    invoke(&cpu, BOW_RT_CLASSIFY); assert_only_result(&cpu, 8);
    assert(next_input.r[0] == scope && next_input.r[1] == hunter && next_input.r[14] == BOW_RT_CLASSIFY_LR);
    cpu = seeded(hunter, remote, BOW_RT_AIM_MODE_LR);
    invoke(&cpu, BOW_RT_AIM_MODE); assert_only_result(&cpu, 1);
    assert(charge_calls == 1);  // the saved hunter survives the next hook clobbering r0 and LR
    const uint32_t callers[] = {0,0x81e294,0x81e520,0x227898,0x4a0824,0x12345678};
    for (unsigned i = 0; i < sizeof callers / sizeof *callers; ++i) {
        cpu = seeded(scope,hunter,callers[i]); invoke(&cpu,BOW_RT_CLASSIFY); assert_only_result(&cpu,0x55);
        cpu = seeded(hunter,0,callers[i]); invoke(&cpu,BOW_RT_AIM_MODE); assert_only_result(&cpu,0x66);
    }
    // The classifier receives h in r1; r0 is its Scope object, never its hunter.
    cpu = seeded(hunter,remote,BOW_RT_CLASSIFY_LR); invoke(&cpu,BOW_RT_CLASSIFY); assert_only_result(&cpu,0x55);
    cpu = seeded(remote,hunter,BOW_RT_AIM_MODE_LR); invoke(&cpu,BOW_RT_AIM_MODE); assert_only_result(&cpu,0x66);
    assert(charge_calls == 1 && classifier_calls == 8 && aim_mode_calls == 8);
    puts("PASS: next hooks always run; exact caller and hunter-register scope; saved arguments survive plugin clobbers; full CPU/VFP state survives the independent charge query.");
}

static void plugin_true_cannot_bypass_hiding(void) {
    world(); install(0); class_result = 2; gate_result = 1; clobber_next = true;
    action(0x10,0xd); assert(screen_reticle(false));
    action(0x10,0); assert(!screen_reticle(false));
    action(0x10,0xd); ST8(hunter+PL_FLAGS,0);
    unsigned before = charge_calls;
    assert(!screen_reticle(false) && charge_calls == before);
    puts("PASS: a plugin returning visible category 2 / gate true cannot keep a released or sheathed local Bow reticle visible.");
}

static void identities(void) {
    const uint32_t kinds[] = {VT_HEAVY,VT_LIGHT,0x1949a88u,0,0x198bf48u};
    world(); install(0); class_result = 0x55; gate_result = 1; action(0x10,0xd);
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        ST32(hunter,kinds[i]);
        if (kinds[i] == 0x1949a88u) ST32(kinds[i]+4*VT_DRAWN,FN_DRAWN);
        Cpu cpu=seeded(scope,hunter,BOW_RT_CLASSIFY_LR); invoke(&cpu,BOW_RT_CLASSIFY); assert_only_result(&cpu,0x55);
        cpu=seeded(hunter,0,BOW_RT_AIM_MODE_LR); invoke(&cpu,BOW_RT_AIM_MODE); assert_only_result(&cpu,1);
    }
    ST32(hunter,VT_BOW);
    const uint32_t invalid[] = {0,remote,3,0xf1000000u};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        Cpu cpu=seeded(scope,invalid[i],BOW_RT_CLASSIFY_LR); invoke(&cpu,BOW_RT_CLASSIFY); assert_only_result(&cpu,0x55);
        cpu=seeded(invalid[i],0,BOW_RT_AIM_MODE_LR); invoke(&cpu,BOW_RT_AIM_MODE); assert_only_result(&cpu,1);
    }
    ST32(G_GAMECONTROL,0);
    Cpu cpu=seeded(scope,hunter,BOW_RT_CLASSIFY_LR); invoke(&cpu,BOW_RT_CLASSIFY); assert_only_result(&cpu,0x55);
    cpu=seeded(hunter,0,BOW_RT_AIM_MODE_LR); invoke(&cpu,BOW_RT_AIM_MODE); assert_only_result(&cpu,1);
    assert(charge_calls == 0);
    world(); install(0);
    // A real existing indirection used by local player slots still resolves normally.
    ST32(LD32(G_SPLAYER)+0x1c,0x08008000); ST32(0x08008000+LINK_A,hunter);
    action(0x10,0xd); assert(screen_reticle(false));
    puts("PASS: remote/invalid/unknown hunters, other weapons and absent scene preserve original results; existing local-slot indirection works.");
}

static void no_guest_writes(void) {
    static unsigned char image_before[sizeof image], heap_before[sizeof heap];
    world(); install(0); action(0x10,0xd);
    memset(G2H(hunter+0x26c0),0x7d,0xa0); ST8(hunter+PL_FLAGS,1);
    memcpy(image_before,image,sizeof image); memcpy(heap_before,heap,sizeof heap);
    assert(screen_reticle(false));
    assert(memcmp(image_before,image,sizeof image) == 0 && memcmp(heap_before,heap,sizeof heap) == 0);
    puts("PASS: all guest image/heap bytes stay unchanged, including aim flag/angles, camera/input canaries; no aim, lock, input or gyro API is linked.");
}

static void installation_failures(void) {
    for (unsigned failure=1; failure<=3; ++failure) {
        world(); install(failure); class_result=2; gate_result=0x66; action(0x10,0xd);
        Cpu cpu=seeded(scope,hunter,BOW_RT_CLASSIFY_LR); invoke(&cpu,BOW_RT_CLASSIFY); assert_only_result(&cpu,2);
        cpu=seeded(hunter,0,BOW_RT_AIM_MODE_LR); invoke(&cpu,BOW_RT_AIM_MODE); assert_only_result(&cpu,0x66);
        assert(classifier_calls == 1 && aim_mode_calls == 1 && charge_calls == 0);
    }
    puts("PASS: either/both hook installation failures leave native/plugin results unchanged; no half-enabled feature.");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1],"--baseline") == 0) {
        world(); action(0x10,0xd);
        unsigned failures = !screen_reticle(false);
        action(0x10,0); native_aim=true; failures += screen_reticle(false);
        printf("EXPECTED_BASELINE_CONTRACT_FAILURES=%u (charging hidden; noncharging manual-aim bypass visible)\n",failures);
        return failures == 2 ? 1 : 2;
    }
    charging_contract(); isolated_calls(); plugin_true_cannot_bypass_hiding(); identities(); no_guest_writes(); installation_failures();
    puts("PASS: bow-charge-reticle-v1 (synthetic host regression; native 2D screen reticle, not a verified ballistic arc; no target/device execution).");
    return 0;
}
