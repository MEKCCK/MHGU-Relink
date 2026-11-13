// Host regression for touch -> action rebinding -> menu/cutscene confirmation.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actions.h"

static uint64_t requested_touch, clock_ns = 1000000000ull;
static int held_key;
static ActSnap staged;
volatile uint64_t g_a_pressed_ns, g_present_count;
static struct { bool valid, down; int x, y; } test_ptr;

static uint64_t plat_now_ns(void) { return clock_ns; }
static void act_wheel_step(void) {}
static bool input_view_dash_zr(void) { return false; }
static bool act_phys_held(void *user, int code) { (void)user; return held_key && code == held_key; }
static int act_sw_sources(void *user, int btn, int dev, int *codes, int max) {
    (void)user;
    if (!max || dev != 1) return 0;
    codes[0] = SDL_SCANCODE_A + btn;
    return 1;
}
static void input_act_stage(const ActSnap *snapshot) { staged = *snapshot; }
static bool real_pad_connected(void) { return false; }
static bool chat_active(void) { return false; }
static uint64_t touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, int *rx, int *ry,
                        float *dx, float *dy) {
    (void)pad_connected; (void)typing; (void)lx; (void)ly; (void)rx; (void)ry; (void)dx; (void)dy;
    *buttons |= requested_touch;
    return requested_touch;
}

// Generated from this checkout's host_input.c, including its real act_update.
#include "host_touch_actions_under_test.h"

static void profile(const char *ini) {
    ActProfile p;
    act_parse(ini, &p);
    act_use_profile(&p);
    held_key = 0;
    requested_touch = 0;
    host_touch_sample(0, 0, UINT64_MAX, false, false);
    g_a_pressed_ns = 0;
}
static void sample(uint64_t touch, uint64_t physical, uint64_t allowed, bool menu, bool typing) {
    requested_touch = touch;
    clock_ns += 16000000ull;
    ++g_present_count;
    host_touch_sample(physical, 0, allowed, menu, typing);
}

int main(void) {
    const int views[] = {ACT_VIEW_MB1, ACT_VIEW_MB2, ACT_VIEW_TOWN};
    const uint32_t engine_bit[] = {0x2000, 0x4000, 0x1000, 0x8000, 0x2, 0x4, 0x100, 0x200,
                                   0x400, 0x800, 0x8, 0x1, 0x80, 0x10, 0x20, 0x40};
    profile("[Actions]\nEnable=1\nManageMenus=1\n");
    for (int btn = 0; btn < 16; ++btn) {
        sample(0, 0, UINT64_MAX, false, false);
        sample(1ull << btn, 0, UINT64_MAX, false, false);
        for (int variant = 0; variant < ACT_VARS; ++variant)
            for (unsigned view = 0; view < sizeof views / sizeof *views; ++view)
                assert(act_view(views[view], engine_bit[btn], staged.hold[variant], false) == engine_bit[btn]);
    }
    sample(0, 0, UINT64_MAX, false, false);
    assert(staged.hold[0] == 0 && staged.pulse[0] == 0);
    sample(1, 0, UINT64_MAX, false, false);
    assert(staged.pulse[0] >> ACT_MENU_CONFIRM & 1);
    assert(g_a_pressed_ns == clock_ns);  // evskip sees the same confirmation edge.
    uint64_t first_press = g_a_pressed_ns;
    sample(1, 0, UINT64_MAX, false, false);
    assert(!(staged.pulse[0] >> ACT_MENU_CONFIRM & 1) && g_a_pressed_ns == first_press);
    sample(0, 0, UINT64_MAX, false, false);
    sample(1, 0, UINT64_MAX, false, false);
    assert(g_a_pressed_ns == clock_ns);

    profile("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nMenuConfirm=None\nAttackA=None\n"
            "[Actions.Kbm]\nMenuConfirm=key:z\nAttackA=key:z\n");
    held_key = SDL_SCANCODE_A;
    sample(0, 1, UINT64_MAX, false, false);
    assert(!(staged.hold[0] >> ACT_MENU_CONFIRM & 1));
    sample(1, 1, UINT64_MAX, false, false);  // Physical A was already down: touch must keep its own source.
    assert(staged.hold[0] >> ACT_MENU_CONFIRM & 1);
    assert(g_a_pressed_ns == clock_ns);
    for (unsigned view = 0; view < sizeof views / sizeof *views; ++view)
        assert(act_view(views[view], 0x2000, staged.hold[0], false) == 0x2000);

    held_key = 0;
    sample(1, 0, UINT64_MAX, true, false);
    assert(staged.hold[0] == 0);  // Host settings own the input.
    sample(1, 0, UINT64_MAX, false, true);
    assert(staged.hold[0] == 0);  // A text entry owns the input.
    sample(1, 0, 0, false, false);
    assert(staged.hold[0] == 0);  // Closing settings must still swallow its confirming finger.
    sample(1, 0, UINT64_MAX, false, false);
    assert(staged.pulse[0] >> ACT_MENU_CONFIRM & 1);
    assert(g_a_pressed_ns == clock_ns);

    sample(0, 0, UINT64_MAX, false, false);
    host_touch_sample(0, 2, UINT64_MAX, false, false);
    assert(act_view(ACT_VIEW_MB2, 0x4000, staged.hold[0], false) == 0x4000);
    puts("PASS: real host touch/action wiring preserves all native buttons, confirmation edges, remapped controls, overlapping physical input and input suppression");
    return 0;
}
