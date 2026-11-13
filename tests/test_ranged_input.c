// Actual lock/aim code against a small synthetic guest world; no game files or live save.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"
#include "../runtime/gyro.h"

static unsigned char image[0x2400000], heap[0x40000];
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

#include "../runtime/hle/mousecam.c"
#define sane weapon_sane
#include "../runtime/hle/weapon.c"
#undef sane

static uint64_t clock_ns = 1000000000ull, buttons;
static int32_t stick_state[4];
static bool locked = true, persistent, native_aim = true;
static float aim_up = 45, aim_down = 30;
uint64_t plat_now_ns(void) { return clock_ns; }
bool touch_lock_on(void) { return locked; }
bool touch_lock_active(void) { return locked; }
int config_live_persistent_lock_on(void) { return persistent; }
void input_get(uint64_t *b, int32_t sticks[4]) { *b = buttons; memcpy(sticks, stick_state, sizeof stick_state); }
float config_live_mouse_smoothing(void) { return 0; }
float config_live_mouse_sensitivity(void) { return 1; }
float config_live_mouse_sensitivity_y(void) { return 1; }
int config_live_mouse_invert_y(void) { return 0; }
float config_motion_gain(void) { return 1; }
void fnhook_call_original(Cpu *c, uint32_t fn) { (void)c; (void)fn; }
void guest_call(Cpu *c, uint32_t fn) {
    if (fn == FN_AIMING) {
        assert(c->r[0] == 0x08004000u);
        assert(c->r[1] == 1 && c->r[2] == 0);
        c->r[0] = native_aim;
    } else c->v.f[0] = c->r[1] == QC_PARAM_AIM_DOWN ? aim_down : aim_up;
}
uint32_t hle_register(const char *name, HostFn fn) { (void)name; (void)fn; return 0; }
bool fnhook_set(uint32_t fn, uint32_t detour) { (void)fn; (void)detour; return true; }

static const uint32_t hunter = 0x08004000, camera = 0x08018000;
static void action(unsigned state, unsigned number) { ST16(hunter + PL_ACT_ST, state); ST16(hunter + PL_ACT_NO, number); }
static void step(uint64_t b) { buttons = b; clock_ns += 16000000; mousecam_lock_shot(); }
static void reset_shot(void) { locked = false; step(0); locked = true; }

static void world(void) {
    memset(stick_state, 0, sizeof stick_state);
    uint32_t gc = 0x08000000, sp = 0x08001000, sen = 0x08003000;
    ST32(G_GAMECONTROL, gc); ST32(G_SPLAYER, sp); ST32(G_SENEMY, sen);
    ST32(gc + 0x30, 0); ST32(sp + 0x1c, hunter);
    ST32(hunter, VT_HEAVY); ST32(VT_HEAVY + 4 * VT_DRAWN, FN_DRAWN);
    ST32(VT_LIGHT + 4 * VT_DRAWN, FN_DRAWN); ST32(VT_BOW + 4 * VT_DRAWN, FN_DRAWN);
    ST8(hunter + PL_FLAGS, 1); ST8(hunter + ZONE, 1); action(1, 0);
    for (int i = 0; i < 2; i++) {
        uint32_t enemy = 0x08009000 + i * 0x2000, info = 0x08010000 + i * 0x2000;
        ST32(sen + 0x1c + 4 * i, enemy); ST32(enemy + 0x1428, info);
        ST16(info + 0x1b6, i + 1); ST8(enemy + ZONE, 1);
        STF(info + 0x40, i ? -1500 : 1000); STF(info + 0x44, 500); STF(info + 0x48, 1000);
    }
    g_cam = camera; ST32(camera + QC_PLAYER, hunter);
}

static void target_switch(void) {
    float target[3];
    assert(lock_target(0, 0, 0, target, true) && target[0] == 1000);
    // Moving nearer the other target does not silently change the selected monster.
    assert(lock_target(-1400, 0, 900, target, true) && target[0] == 1000);
    mousecam_target_cycle(); clock_ns += 16000000;
    assert(lock_target(0, 0, 0, target, true) && target[0] == -1500);
    assert(lock_target(0, 0, 0, target, true) && target[0] == -1500);
    mousecam_target_cycle(); clock_ns += 16000000;
    assert(lock_target(0, 0, 0, target, true) && target[0] == 1000);
    // A selected monster moving to another zone falls back to the valid one.
    ST8(0x08009000 + ZONE, 2);
    assert(lock_target(0, 0, 0, target, true) && target[0] == -1500);
    ST8(0x08009000 + ZONE, 1);
    mousecam_target_cycle(); clock_ns += 16000000;
    assert(lock_target(0, 0, 0, target, true) && target[0] == 1000);
}

static void shots(void) {
    const uint64_t A = 1ull << BTN_A, X = 1ull << BTN_X, R = 1ull << BTN_R;
    for (int i = 0; i < 2; i++) {
        ST32(hunter, i ? VT_LIGHT : VT_HEAVY); action(1, 0); reset_shot();
        ST32(hunter + PL_AIM_YAW, 0); STF(hunter + PL_AIM_PITCH, -20);
        step(A);
        assert(LD32(hunter + PL_AIM_YAW) == 8192 && LDF(hunter + PL_AIM_PITCH) == -20);
        assert(buttons == A);  // no scope, reload or crouch is synthesized
        action(1, 46); ST32(hunter + PL_AIM_YAW, 777); step(A | R);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // manual aiming is yours
        action(1, 0); step(A | R);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // input takes priority before the aim action starts
        action(1, 0); ST32(hunter + PL_AIM_YAW, 888); step(X | A);
        assert(LD32(hunter + PL_AIM_YAW) == 888);  // native crouching input is yours
    }
    ST32(hunter, VT_BOW); reset_shot(); action(1, 0);
    ST32(hunter + PL_AIM_YAW, 0); step(X);
    assert(LD32(hunter + PL_AIM_YAW) == 8192 && LDF(hunter + PL_AIM_PITCH) > 0);
    action(0x10, 8); ST32(hunter + PL_AIM_YAW, 0); step(0);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);  // release after a normal charge stays directed
    reset_shot(); action(1, 0); step(0); action(0x10, 8); ST32(hunter + PL_AIM_YAW, 0); step(X);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);  // the game may enter charge before the input hook observes the press
    reset_shot(); action(1, 85); ST32(hunter + PL_AIM_YAW, 999); step(X);
    assert(LD32(hunter + PL_AIM_YAW) == 999);  // knockback cannot start charge assistance
    reset_shot(); action(0x10, 99); step(X);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);  // a held charge can face the target without idle provenance
    reset_shot(); action(1, 0); ST32(hunter + PL_AIM_YAW, 999); step(A);
    assert(LD32(hunter + PL_AIM_YAW) == 999);  // bow melee is not a charge
    ST8(LD32(G_GAMECONTROL) + 0x6c, 1); reset_shot(); step(R);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);  // Type 2 draws with R
    ST8(LD32(G_GAMECONTROL) + 0x6c, 0);
}

static void bow_lock_enable(int type) {
    const uint64_t fire = 1ull << (type ? BTN_R : BTN_X);
    ST32(hunter, VT_BOW); ST8(LD32(G_GAMECONTROL) + 0x6c, type);
    ST8(hunter + PL_FLAGS, 1); action(1, 0); locked = false; step(0);
    // The native update can enter charge between the last unlocked idle and the first locked sample.
    action(0x10, 8); ST32(hunter + PL_AIM_YAW, 777); locked = true; step(fire);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);

    // Locking during a held charge applies the same current-input rule; no new press is required.
    action(1, 0); locked = false; step(0);
    ST32(hunter + PL_AIM_YAW, 777); STF(hunter + PL_AIM_PITCH, 12);
    STF(hunter + PL_AIM_PITCH2, -13); ST16(hunter + PL_AIM_LEAN, 100);
    action(0x10, 8); step(fire);
    assert(LD32(hunter + PL_AIM_YAW) == 777 && LDF(hunter + PL_AIM_PITCH) == 12);
    assert(LDF(hunter + PL_AIM_PITCH2) == -13 && LD16(hunter + PL_AIM_LEAN) == 100);
    locked = true; step(fire);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);
    action(1, 0); reset_shot(); ST8(LD32(G_GAMECONTROL) + 0x6c, 0);
    printf("PASS: Bow Type %d lock activation during charge, including fire already held; unlocked angles unchanged.\n", type + 1);
}

static void shot_left_stick(uint32_t weapon, int type) {
    const uint64_t fire = 1ull << (weapon != VT_BOW ? BTN_A : type ? BTN_R : BTN_X);
    const int32_t pushed[][2] = {{3000, 0}, {-3000, 0}, {0, 3000}, {0, -3000}, {20000, -12000}};
    world(); ST32(hunter, weapon); ST8(LD32(G_GAMECONTROL) + 0x6c, type);
    g_aim_seen_ns = 0; reset_shot(); step(fire); action(0x10, 8);
    for (unsigned i = 0; i < sizeof pushed / sizeof *pushed; ++i) {
        stick_state[0] = pushed[i][0]; stick_state[1] = pushed[i][1];
        ST32(hunter + PL_AIM_YAW, 777); STF(hunter + PL_AIM_PITCH, 12);
        STF(hunter + PL_AIM_PITCH2, -13); ST16(hunter + PL_AIM_LEAN, 100);
        step(fire);
        assert(LD32(hunter + PL_AIM_YAW) == 777 && LDF(hunter + PL_AIM_PITCH) == 12);
        assert(LDF(hunter + PL_AIM_PITCH2) == -13 && LD16(hunter + PL_AIM_LEAN) == 100);
        stick_state[0] = stick_state[1] = 0; step(fire);
        assert(LD32(hunter + PL_AIM_YAW) == 8192);  // the same valid charge resumes without another press
    }
    stick_state[0] = 2999; stick_state[1] = -2999;
    stick_state[2] = 32767; stick_state[3] = -32767;
    ST32(hunter + PL_AIM_YAW, 777); step(fire);
    assert(LD32(hunter + PL_AIM_YAW) == 8192);  // drift and the right stick do not pause automatic facing
    memset(stick_state, 0, sizeof stick_state);
    if (weapon == VT_BOW) {
        // A moving start becomes eligible immediately when the stick centers during the same held charge.
        action(2, 0); reset_shot(); stick_state[0] = 20000; ST32(hunter + PL_AIM_YAW, 777); step(fire);
        assert(LD32(hunter + PL_AIM_YAW) == 777);
        action(0x10, 8); step(fire); assert(LD32(hunter + PL_AIM_YAW) == 777);
        stick_state[0] = 0; step(fire); assert(LD32(hunter + PL_AIM_YAW) == 8192);
        const uint64_t cancel[] = {1ull << BTN_B, 1ull << BTN_Y, 1ull << BTN_A, 1ull << (type ? BTN_X : BTN_R)};
        for (unsigned i = 0; i < sizeof cancel / sizeof *cancel; ++i) {
            action(1, 0); reset_shot(); step(fire); action(0x10, 8);
            stick_state[0] = 20000; ST32(hunter + PL_AIM_YAW, 777); step(fire | cancel[i]);
            stick_state[0] = 0; step(fire);
            assert(LD32(hunter + PL_AIM_YAW) == 777);  // cancellation during movement still ends the charge assist
        }
        action(1, 0); reset_shot(); step(fire); action(0x10, 8);
        stick_state[0] = 20000; ST8(hunter + PL_FLAGS, 0); ST32(hunter + PL_AIM_YAW, 777); step(fire);
        stick_state[0] = 0; ST8(hunter + PL_FLAGS, 1); step(fire);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // sheathing cannot be undone by just centering the stick
        action(1, 0); reset_shot(); step(fire);
        action(1, 46); ST32(hunter + PL_AIM_YAW, 777); step(fire);
        action(0x10, 8); step(fire);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // leaving manual aim cannot resurrect a cancelled held charge
        action(1, 0); reset_shot(); step(fire); action(0x10, 8);
        stick_state[0] = 20000; ST32(hunter + PL_AIM_YAW, 777); step(0);
        stick_state[0] = 0; step(0); assert(LD32(hunter + PL_AIM_YAW) == 8192);  // release is still a live shot
        clock_ns += SHOT_NS; ST32(hunter + PL_AIM_YAW, 777);
        stick_state[0] = 0; step(0);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // returning to center cannot revive an expired release
    }
    ST8(LD32(G_GAMECONTROL) + 0x6c, 0); action(1, 0); reset_shot();
    printf("PASS: %s Type %d ordinary shot pauses for left-stick movement, resumes at center, ignores drift/right stick and preserves cancellation.\n",
           weapon == VT_BOW ? "Bow" : weapon == VT_HEAVY ? "HBG" : "LBG", type + 1);
}

static void shot_identity_reset(void) {
    const uint64_t X = 1ull << BTN_X;
    world(); ST32(hunter, VT_BOW); action(1, 0); g_aim_seen_ns = 0; reset_shot(); step(X); action(0x10, 8);
    const uint32_t other = 0x08030000, slot = LD32(G_SPLAYER) + 0x1c;
    memcpy(G2H(other), G2H(hunter), 0x3000); ST32(slot, other);
    ST32(other + PL_AIM_YAW, 777); step(0);
    assert(LD32(other + PL_AIM_YAW) == 777);  // no old release window crosses hunters
    ST32(slot, hunter); action(1, 0); reset_shot(); step(X); action(0x10, 8);
    ST32(hunter, VT_HEAVY); ST32(hunter + PL_AIM_YAW, 777); step(0);
    assert(LD32(hunter + PL_AIM_YAW) == 777);  // or a weapon switch on the same hunter
    ST32(hunter, VT_BOW); action(1, 0); reset_shot(); step(X); action(0x10, 8);
    ST32(hunter, 0); step(0); ST32(hunter, VT_BOW); ST32(hunter + PL_AIM_YAW, 777); step(0);
    assert(LD32(hunter + PL_AIM_YAW) == 777);
    const unsigned common[] = {0x00010017u, 0x00010055u, 0x00020000u};
    for (unsigned i = 0; i < sizeof common / sizeof *common; ++i) {
        action(common[i] >> 16, common[i] & 0xffff); ST32(hunter + PL_AIM_YAW, 777); step(X);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // no facing in a non-shooting common action
    }
    action(1, 0); reset_shot(); ST32(hunter + PL_AIM_YAW, 777); step(0);
    assert(LD32(hunter + PL_AIM_YAW) == 777);  // merely being idle never turns the hunter
    puts("PASS: no released-shot leakage across hunter/weapon/non-ranged changes; non-shooting common actions and idle stay manual.");
}

static void actual_aim(void) {
    ST32(hunter, VT_LIGHT); action(0x10, 99);
    assert(!weapon_aiming(hunter));
    Cpu cpu = {0}; cpu.r[0] = hunter; g_seen_ns = 0;
    aim_turn(&cpu); assert(weapon_aiming(hunter));  // scope/style actually ran the native aim handler
    assert(mousecam_active());  // no motion or touch input is needed to establish aim-camera liveness
    action(0x10, 4); assert(weapon_aiming(hunter));  // brief scope recoil
    action(1, 0); assert(!weapon_aiming(hunter));
    action(0x10, 99); clock_ns += 150000000; assert(!weapon_aiming(hunter));
    action(1, 46); assert(weapon_aiming(hunter));  // known aiming state needs no host motion to start
}

static void move_body(int slot, float x, float y, float z) {
    uint32_t enemy = 0x08009000 + slot * 0x2000, info = 0x08010000 + slot * 0x2000;
    uint32_t joints = 0x08020000 + slot * 0x800;
    STF(info + 0x40, x); STF(info + 0x44, y); STF(info + 0x48, z);
    ST32(enemy + EN_JOINTS, 3); ST32(enemy + EN_JOINTS + 4, joints);
    for (int i = 0; i < 3; ++i) {
        uint32_t joint = joints + i * JOINT_SIZE;
        ST32(joint, 0x00300000); ST8(joint + 0x91, i ? 0 : 0xff);
        STF(joint + JOINT_POS, x + 100); STF(joint + JOINT_POS + 4, y + (i == 0 ? 180 : i == 1 ? 0 : 240));
        STF(joint + JOINT_POS + 8, z + 50); STF(joint + JOINT_POS + 12, 1);
    }
}
static void scope_frame(int mode) {
    Cpu cpu = {0}; cpu.r[0] = hunter; cpu.r[1] = (uint32_t)mode;
    clock_ns += 16000000; aim_turn(&cpu);
}
static void assert_scope_target(float x, float y, float z, int mode) {
    int yaw = (int)lrintf(atan2f(x, z) * UNITS_PER_RAD) & 0xffff;
    int up = (int)(aim_up * UNITS_PER_DEG + .5f), down = (int)(aim_down * UNITS_PER_DEG + .5f);
    int pitch = (int)lrintf(atan2f(y - MUZZLE_Y, hypotf(x, z)) * UNITS_PER_RAD);
    if (pitch > up) pitch = up;
    if (pitch < -down) pitch = -down;
    if (mode == 1 && pitch > AIM_LEAN_MAX) pitch = AIM_LEAN_MAX;
    if (mode == 1 && pitch < -AIM_LEAN_MAX) pitch = -AIM_LEAN_MAX;
    assert(LD32(hunter + PL_AIM_YAW) == (uint32_t)yaw);
    float aim = LDF(hunter + PL_AIM_PITCH);
    assert(aim == LDF(hunter + PL_AIM_PITCH2) && aim >= -100 && aim <= 100);
    assert(fabsf(-down + (aim + 100) * (up + down) / 200 - pitch) < .01f);
    assert((int16_t)LD16(hunter + PL_AIM_LEAN) == pitch);
    // Camera geometry is owned by the native camera; tracking only supplies the hunter's aim.
}
static void scope_left_stick(uint32_t weapon) {
    const int32_t pushed[][2] = {{3000, 0}, {-3000, 0}, {0, 3000}, {0, -3000}, {20000, -12000}};
    for (int mode = 0; mode < 2; ++mode) {
        world(); ST32(hunter, weapon); action(1, 46);
        g_lock_en = g_target_id = 0; g_lock_slot = -1; g_cycle_req = 0;
        persistent = locked = native_aim = true; g_seen_ns = clock_ns;
        move_body(0, 1000, 500, 1000); move_body(1, -1500, 500, 1000);
        scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        for (unsigned i = 0; i < sizeof pushed / sizeof *pushed; ++i) {
            stick_state[0] = pushed[i][0]; stick_state[1] = pushed[i][1];
            ST32(hunter + PL_AIM_YAW, 777); STF(hunter + PL_AIM_PITCH, 12);
            STF(hunter + PL_AIM_PITCH2, -13); ST16(hunter + PL_AIM_LEAN, 100);
            unsigned char camera_before[0xb00]; memcpy(camera_before, G2H(camera), sizeof camera_before);
            scope_frame(mode);
            assert(LD32(hunter + PL_AIM_YAW) == 777 && LDF(hunter + PL_AIM_PITCH) == 12);
            assert(LDF(hunter + PL_AIM_PITCH2) == -13 && LD16(hunter + PL_AIM_LEAN) == 100);
            assert(!memcmp(camera_before, G2H(camera), sizeof camera_before));
            stick_state[0] = stick_state[1] = 0;
            scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        }
        stick_state[0] = 2999; stick_state[1] = -2999;
        stick_state[2] = 32767; stick_state[3] = -32767;
        ST32(hunter + PL_AIM_YAW, 777);
        scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        uint32_t target_yaw = LD32(hunter + PL_AIM_YAW);
        memset(stick_state, 0, sizeof stick_state);
        stick_state[0] = 20000; ST32(hunter + PL_AIM_YAW, 777); mousecam_add(8, -6); mousecam_motion_add(.02f, .01f);
        scope_frame(mode);
        assert(LD32(hunter + PL_AIM_YAW) != 777 && LD32(hunter + PL_AIM_YAW) != target_yaw);  // movement preserves manual drag/gyro
        stick_state[0] = 0; scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        uint64_t fire = 1ull << (weapon == VT_BOW ? BTN_X : BTN_A);
        const uint64_t cancel[] = {1ull << BTN_B, 1ull << BTN_Y, 1ull << (weapon == VT_BOW ? BTN_A : BTN_X)};
        for (unsigned i = 0; i < sizeof cancel / sizeof *cancel; ++i) {
            buttons = fire | cancel[i]; ST32(hunter + PL_AIM_YAW, 777); scope_frame(mode);
            assert(LD32(hunter + PL_AIM_YAW) == 777);
            buttons = fire; scope_frame(mode);
            assert(LD32(hunter + PL_AIM_YAW) == 777);
            buttons = 0; scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        }
        // Both native hook orders must share explicit cancellation, without treating scoped firing as cancellation.
        step(fire); scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        ST32(hunter + PL_AIM_YAW, 777); step(fire | (1ull << BTN_B)); buttons = fire; scope_frame(mode);
        assert(LD32(hunter + PL_AIM_YAW) == 777);
        buttons = 0; scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        ST32(hunter + PL_AIM_YAW, 777); buttons = fire | (1ull << BTN_B); scope_frame(mode); step(fire); scope_frame(mode);
        assert(LD32(hunter + PL_AIM_YAW) == 777);
        step(0); scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        buttons = fire; ST8(hunter + PL_FLAGS, 0); ST32(hunter + PL_AIM_YAW, 777); scope_frame(mode);
        assert(LD32(hunter + PL_AIM_YAW) == 777);
        ST8(hunter + PL_FLAGS, 1); scope_frame(mode);
        assert(LD32(hunter + PL_AIM_YAW) == 777);
        buttons = 0; scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
        native_aim = false; ST32(hunter + PL_AIM_YAW, 777); scope_frame(mode);
        assert(LD32(hunter + PL_AIM_YAW) == 777);  // center cannot open or restore a closed native scope
    }
    memset(stick_state, 0, sizeof stick_state); persistent = false;
    printf("PASS: %s persistent scope, both modes: left-stick pause/center resume, drift/right stick ignored, camera unchanged.\n",
           weapon == VT_BOW ? "Bow" : weapon == VT_HEAVY ? "HBG" : "LBG");
}
static void no_unscoped_steering(void) {
    const uint32_t weapons[] = {VT_HEAVY, VT_LIGHT, VT_BOW};
    for (unsigned w = 0; w < sizeof weapons / sizeof *weapons; ++w) {
        world(); ST32(hunter, weapons[w]); persistent = locked = true; native_aim = false;
        move_body(0, 30, 0, 50);
        for (int mode = 0; mode <= 6; ++mode) {
            action(1, 0); ST32(hunter + PL_AIM_YAW, 777);
            STF(hunter + PL_AIM_PITCH, 12); STF(hunter + PL_AIM_PITCH2, 12);
            ST16(hunter + PL_AIM_LEAN, 100);
            unsigned char before[0xb00]; memcpy(before, G2H(camera), sizeof before);
            scope_frame(mode);
            assert(LD32(hunter + PL_AIM_YAW) == 777 && LDF(hunter + PL_AIM_PITCH) == 12);
            assert(LDF(hunter + PL_AIM_PITCH2) == 12 && LD16(hunter + PL_AIM_LEAN) == 100);
            assert(!memcmp(before, G2H(camera), sizeof before) && !mousecam_hunter_aiming(hunter));
        }
        native_aim = true; action(1, 46); move_body(0, 1000, 500, 1000);
        unsigned char camera_before[0xb00], position_before[12];
        memcpy(camera_before, G2H(camera), sizeof camera_before);
        memcpy(position_before, G2H(hunter + PL_PLACE), sizeof position_before);
        scope_frame(0); assert_scope_target(1100, 680, 1050, 0);
        assert(!memcmp(camera_before, G2H(camera), sizeof camera_before));
        assert(!memcmp(position_before, G2H(hunter + PL_PLACE), sizeof position_before));
        // A call immediately after closing the scope must clear liveness, not inherit the previous frame's aim.
        native_aim = false; action(1, 0); ST32(hunter + PL_AIM_YAW, 321); scope_frame(0);
        assert(LD32(hunter + PL_AIM_YAW) == 321 && !mousecam_hunter_aiming(hunter));
    }
    native_aim = true; persistent = false;
    puts("PASS: both locks, all ranged weapons/modes: no unscoped steering, no hunter/camera translation or camera-state writes; immediate scope close.");
}
static void persistent_scope(void) {
    const uint32_t weapons[] = {VT_HEAVY, VT_LIGHT, VT_BOW};
    for (unsigned w = 0; w < sizeof weapons / sizeof *weapons; ++w)
        for (int mode = 0; mode < 2; ++mode) {
            aim_up = mode == 1 ? 60 : 45; aim_down = mode == 1 ? 55 : 30;
            world(); ST32(hunter, weapons[w]); action(1, 46);
            g_lock_en = g_target_id = 0; g_lock_slot = -1; g_cycle_req = 0;
            persistent = locked = true; g_seen_ns = clock_ns;
            move_body(0, 1000, 500, 1000); move_body(1, -1500, 500, 1000);
            scope_frame(mode); assert_scope_target(1100, 680, 1050, mode);
            // The same selected monster moves, including its skeleton, without another input or fire press.
            move_body(0, -800, 100, 1400);
            scope_frame(mode); assert_scope_target(-700, 280, 1450, mode);
            mousecam_target_cycle(); scope_frame(mode);
            assert_scope_target(-1400, 680, 1050, mode);
            mousecam_target_cycle(); scope_frame(mode);
            assert_scope_target(-700, 280, 1450, mode);
            mousecam_add(25, -30); mousecam_motion_add(.1f, .15f);
            scope_frame(mode); assert_scope_target(-700, 280, 1450, mode);
            assert(!g_dx && !g_dy && !g_mot_yaw && !g_mot_pitch && !g_rest_yaw && !g_rest_pitch);
            // Some Bow control types run the camera stick hook first: it must not skip absolute aim tracking.
            Cpu cam_cpu = {0}; cam_cpu.r[0] = camera;
            mousecam_add(15, 20); camera_stick(&cam_cpu);
            scope_frame(mode); assert_scope_target(-700, 280, 1450, mode);
            move_body(0, 0, 6000, 1400);
            scope_frame(mode); assert_scope_target(100, 6180, 1450, mode);
            move_body(0, 0, -6000, 1400);
            scope_frame(mode); assert_scope_target(100, -5820, 1450, mode);
            // Manual close must stop tracking and must not replay the drag or gyro from the locked frame.
            locked = false; ST32(hunter + PL_AIM_YAW, 777); STF(hunter + PL_AIM_PITCH, 12);
            uint32_t eye_yaw = LD32(camera + QC_EYE_YAW);
            scope_frame(mode); assert(LD32(hunter + PL_AIM_YAW) == 777 && LDF(hunter + PL_AIM_PITCH) == 12);
            assert(LD32(camera + QC_EYE_YAW) == eye_yaw);
            // Ordinary lock-on keeps the original manual scope, including fresh drag and gyro input.
            persistent = false; locked = true; move_body(0, 900, 500, 900);
            scope_frame(mode); assert(LD32(hunter + PL_AIM_YAW) == 777);
            mousecam_add(8, -6); mousecam_motion_add(.02f, .01f); scope_frame(mode);
            uint32_t manual_yaw = LD32(hunter + PL_AIM_YAW); assert(manual_yaw != 777);
            scope_frame(mode); assert(LD32(hunter + PL_AIM_YAW) == manual_yaw);
            persistent = true;
            ST8(0x08009000 + ZONE, 2); ST8(0x0800b000 + ZONE, 2);
            scope_frame(mode); assert(LD32(hunter + PL_AIM_YAW) == manual_yaw);  // no valid target: keep manual aim
        }
    persistent = false; aim_up = 45; aim_down = 30;
    puts("PASS: persistent HBG/LBG/Bow scope tracks moving body, target cycles and native pitch limits; manual close, gyro/drag consumption and ordinary aim preserved.");
}

static void relative_gyro(void) {
    GyroTurn turn = {0};
    gyro_sample(&turn, 1000000000, 1, 2); assert(turn.yaw == 0 && turn.pitch == 0);
    gyro_sample(&turn, 1010000000, 1, 2);
    assert(fabsf(turn.yaw - .01f) < 1e-6f && fabsf(turn.pitch - .02f) < 1e-6f);
    gyro_sample(&turn, 1010000000, 1, 2); assert(fabsf(turn.yaw - .01f) < 1e-6f); // duplicate not replayed
    gyro_sample(&turn, 1020000000, .01f, -.01f); assert(fabsf(turn.yaw - .01f) < 1e-6f); // dead band
    gyro_sample(&turn, 1030000000, NAN, 1); assert(turn.sample_ns == 1020000000);
    gyro_sample(&turn, 2000000000, 10, 10); assert(turn.yaw == 0 && turn.pitch == 0); // pause makes a baseline
    turn = (GyroTurn){0}; gyro_sample(&turn, 3000000000, 5, 5);
    assert(turn.yaw == 0 && turn.pitch == 0); // source/aim restart cannot jump
    // A phone turn and a finger drag add at the current camera angle, with the same signs.
    clock_ns += 16000000; g_seen_ns = clock_ns; mousecam_motion_add(.02f, .01f); mousecam_add(5, -4);
    int yaw, pitch; assert(take_turn(0, &yaw, &pitch));
    assert(yaw < -200 && pitch > 150);
    assert(!take_turn(0, &yaw, &pitch));  // consumed once
}

int main(void) {
    world(); target_switch(); shots(); bow_lock_enable(0); bow_lock_enable(1);
    shot_left_stick(VT_HEAVY, 0); shot_left_stick(VT_LIGHT, 0); shot_left_stick(VT_BOW, 0); shot_left_stick(VT_BOW, 1);
    shot_identity_reset();
    actual_aim(); persistent_scope(); scope_left_stick(VT_HEAVY); scope_left_stick(VT_LIGHT); scope_left_stick(VT_BOW);
    no_unscoped_steering(); relative_gyro();
    puts("PASS: sticky target/cycle, standing bowgun fire, manual aim/crouch, bow charge gates, actual aim states, relative gyro.");
    return 0;
}
