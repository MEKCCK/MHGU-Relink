// Host regression for touch -> action rebinding -> menu/cutscene confirmation.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actions.h"
#include "hle/input.h"

#define B(x) (1ull << (x))

static uint64_t requested_touch, requested_mix_button = B(BTN_A), host_buttons, clock_ns = 1000000000ull;
static bool mapping_active, dash_zr, chatting, window_away, mix_armed;
static int held_key, active_weapon = -1, requested_axes[4];
static unsigned mix_button_calls;
static ActSnap staged;
volatile uint64_t g_a_pressed_ns, g_present_count;
static struct { bool valid, down; int x, y; } test_ptr;

static uint64_t plat_now_ns(void) { return clock_ns; }
static void act_wheel_step(void) {}
static bool input_view_dash_zr(void) { return dash_zr; }
static int input_view_weapon(void) { return active_weapon; }
static bool padmap_active(void) { return mapping_active; }
static bool ui_window_away(void) { return window_away; }
static void menuptr_mix_cancel(void) { mix_armed = false; }
static uint64_t menuptr_mix_buttons(bool cancel) {
    ++mix_button_calls;
    if (cancel) menuptr_mix_cancel();
    return mix_armed ? requested_mix_button : 0;
}
static bool act_phys_held(void *user, int code) { (void)user; return held_key && code == held_key; }
static int act_sw_sources(void *user, int btn, int dev, int *codes, int max) {
    (void)user;
    if (!max || dev != 1) return 0;
    codes[0] = SDL_SCANCODE_A + btn;
    return 1;
}
void input_act_stage(const ActSnap *snapshot) { staged = *snapshot; }
static bool real_pad_connected(void) { return false; }
static bool chat_active(void) { return chatting; }
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
    requested_mix_button = B(BTN_A);
    mapping_active = dash_zr = chatting = window_away = mix_armed = false;
    active_weapon = -1;
    memset(requested_axes, 0, sizeof requested_axes);
    host_touch_sample(0, 0, UINT64_MAX, false, false);
    g_a_pressed_ns = 0;
}
static void sample(uint64_t touch, uint64_t physical, uint64_t allowed, bool menu, bool typing) {
    requested_touch = touch;
    clock_ns += 16000000ull;
    ++g_present_count;
    host_touch_sample(physical, 0, allowed, menu, typing);
}

static void expect_r(bool held) {
    const int views[] = {ACT_VIEW_MB1, ACT_VIEW_SHEATHED, ACT_VIEW_HUNT_ANY, ACT_VIEW_MB2, ACT_VIEW_TOWN};
    for (int variant = 0; variant < ACT_VARS; ++variant)
        for (unsigned view = 0; view < sizeof views / sizeof *views; ++view)
            assert(act_view(views[view], 0x200, staged.hold[variant], false) == (held ? 0x200 : 0));
}

static void mix_auto_confirm(void) {
    profile("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nMenuConfirm=None\n"
            "[Actions.Kbm]\nMenuConfirm=key:z\n");
    mix_armed = true;
    unsigned calls = mix_button_calls;
    sample(0, 0, UINT64_MAX, false, false);
    assert(host_buttons == B(BTN_A) && mix_button_calls == calls + 1);
    for (int variant = 0; variant < ACT_VARS; ++variant) {
        assert(staged.hold[variant] & (1ull << ACT_MENU_CONFIRM));
        assert(!(staged.pulse[variant] & (1ull << ACT_MENU_CONFIRM)));
    }
    assert(g_a_pressed_ns == 0);  // Automatic holds do not become cutscene-skip edges.
    sample(0, 0, UINT64_MAX, false, false);
    assert(host_buttons == B(BTN_A) && g_a_pressed_ns == 0);

    held_key = SDL_SCANCODE_Z;
    sample(0, B(BTN_X) | B(BTN_Y) | B(BTN_LSTICK) | B(BTN_RSTICK), UINT64_MAX, false, false);
    assert(host_buttons == (B(BTN_A) | B(BTN_X) | B(BTN_Y) | B(BTN_LSTICK) | B(BTN_RSTICK)));
    assert(staged.pulse[0] & (1ull << ACT_MENU_CONFIRM));  // Real actions are sampled once, before injection.
    assert(g_a_pressed_ns == clock_ns);
    sample(0, B(BTN_A), UINT64_MAX, false, false);
    assert(!(staged.pulse[0] & (1ull << ACT_MENU_CONFIRM)));
    menuptr_mix_cancel();
    sample(0, B(BTN_A), UINT64_MAX, false, false);
    assert(host_buttons == B(BTN_A) && (staged.hold[0] & (1ull << ACT_MENU_CONFIRM)));

    held_key = 0;
    const int navigation[] = {BTN_B, BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN, BTN_L, BTN_R,
                              BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS};
    for (unsigned i = 0; i < sizeof navigation / sizeof *navigation; ++i) {
        mix_armed = true;
        sample(0, B(navigation[i]), UINT64_MAX, false, false);
        assert(!mix_armed && host_buttons == B(navigation[i]));
        sample(0, 0, UINT64_MAX, false, false);
        assert(host_buttons == 0);  // Cancelling must not resume on its release.
    }
    mix_armed = true;
    sample(B(BTN_B), B(BTN_A), UINT64_MAX, false, false);
    assert(!mix_armed && host_buttons == (B(BTN_A) | B(BTN_B)));  // Preserve both real sources.

    for (int axis = 0; axis < 4; ++axis)
        for (int sign = -1; sign <= 1; sign += 2) {
            mix_armed = true;
            requested_axes[axis] = sign * 12000;
            sample(0, 0, UINT64_MAX, false, false);
            assert(mix_armed && host_buttons == B(BTN_A));
            requested_axes[axis] = sign * 12001;
            sample(0, 0, UINT64_MAX, false, false);
            assert(!mix_armed && host_buttons == 0);
            requested_axes[axis] = 0;
        }

    for (int owner = 0; owner < 5; ++owner) {
        mix_armed = true;
        chatting = owner == 2;
        mapping_active = owner == 3;
        window_away = owner == 4;
        sample(0, 0, UINT64_MAX, owner == 0, owner == 1);
        assert(!mix_armed && host_buttons == 0);
        chatting = mapping_active = window_away = false;
        sample(0, 0, UINT64_MAX, false, false);
        assert(host_buttons == 0);
    }
    mix_armed = true;
    host_touch_focus_lost();
    assert(!mix_armed);
    sample(0, 0, UINT64_MAX, false, false);
    assert(host_buttons == 0);
    puts("PASS: auto combine uses an isolated current-sample A/Confirm hold and cancels on native navigation, either stick, input ownership, background and focus loss");

    profile("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nMenuConfirm=None\nMenuCancel=None\n"
            "[Actions.Kbm]\nMenuConfirm=key:z\nMenuCancel=key:x\n");
    requested_mix_button = B(BTN_B);
    mix_armed = true;
    for (int frame = 0; frame < 2; ++frame) {
        sample(0, 0, UINT64_MAX, false, false);
        assert(mix_armed && host_buttons == B(BTN_B));  // Automatic B cannot cancel its own sequence.
        for (int variant = 0; variant < ACT_VARS; ++variant) {
            assert(staged.hold[variant] & (1ull << ACT_MENU_CANCEL));
            assert(!(staged.hold[variant] & (1ull << ACT_MENU_CONFIRM)));
            assert(!(staged.pulse[variant] & ((1ull << ACT_MENU_CONFIRM) | (1ull << ACT_MENU_CANCEL))));
        }
    }
    assert(g_a_pressed_ns == 0);
    requested_mix_button = 0;
    sample(0, 0, UINT64_MAX, false, false);
    assert(mix_armed && host_buttons == 0);
    for (int variant = 0; variant < ACT_VARS; ++variant)
        assert(!(staged.hold[variant] & ((1ull << ACT_MENU_CONFIRM) | (1ull << ACT_MENU_CANCEL))));
    requested_mix_button = B(BTN_A);
    sample(0, 0, UINT64_MAX, false, false);
    assert(host_buttons == B(BTN_A));
    requested_mix_button = B(BTN_B);
    held_key = SDL_SCANCODE_Z;
    sample(0, B(BTN_A), UINT64_MAX, false, false);
    assert(mix_armed && host_buttons == (B(BTN_A) | B(BTN_B)));
    assert((staged.hold[0] & ((1ull << ACT_MENU_CONFIRM) | (1ull << ACT_MENU_CANCEL))) ==
           ((1ull << ACT_MENU_CONFIRM) | (1ull << ACT_MENU_CANCEL)));
    assert(staged.pulse[0] & (1ull << ACT_MENU_CONFIRM));
    assert(!(staged.pulse[0] & (1ull << ACT_MENU_CANCEL)));
    held_key = SDL_SCANCODE_X;
    sample(0, B(BTN_X), UINT64_MAX, false, false);
    assert(!mix_armed && host_buttons == B(BTN_X));
    assert(staged.pulse[0] & (1ull << ACT_MENU_CANCEL));  // Real remapped cancellation retains its edge.
    held_key = 0;
    mix_armed = true;
    sample(0, B(BTN_B), UINT64_MAX, false, false);
    assert(!mix_armed && host_buttons == B(BTN_B));
    puts("PASS: auto combine A/B holds release immediately, preserve real overlapping inputs and remapped action edges, and never self-cancel");

    const char *const action[] = {"MenuCancel", "MenuUp", "MenuDown", "MenuLeft", "MenuRight",
                                  "MenuTabL", "MenuTabR", "MenuPageL", "MenuPageR", "MenuPlus", "MenuMinus"};
    const int action_bit[] = {ACT_MENU_CANCEL, ACT_MENU_UP, ACT_MENU_DOWN, ACT_MENU_LEFT, ACT_MENU_RIGHT,
                              ACT_MENU_TAB_L, ACT_MENU_TAB_R, ACT_MENU_PAGE_L, ACT_MENU_PAGE_R, ACT_MENU_PLUS, ACT_MENU_MINUS};
    for (unsigned i = 0; i < sizeof action / sizeof *action; ++i) {
        char ini[192];
        snprintf(ini, sizeof ini, "[Actions]\nEnable=1\nManageMenus=1\n[Actions.Kbm]\n%s=key:z\n", action[i]);
        profile(ini);
        mix_armed = true;
        held_key = SDL_SCANCODE_Z;
        sample(0, B(BTN_X), UINT64_MAX, false, false);
        assert(!mix_armed && host_buttons == B(BTN_X));
        assert(staged.pulse[0] & (1ull << action_bit[i]));
    }
    profile("[Actions]\nEnable=1\nManageMenus=0\n[Actions.Kbm]\nMenuCancel=key:z\n");
    mix_armed = true;
    held_key = SDL_SCANCODE_Z;
    sample(0, 0, UINT64_MAX, false, false);
    assert(mix_armed && host_buttons == B(BTN_A));  // Menu bindings cancel only while they own native menus.

    profile("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Kbm]\nMenuCancel=key:z\n"
            "[Actions.Kbm.Weapon.Hammer]\nMenuCancel=key:x\n");
    active_weapon = 2;
    held_key = SDL_SCANCODE_Z;
    mix_armed = true;
    sample(0, 0, UINT64_MAX, false, false);
    assert(mix_armed);  // Inactive base/weapon bindings cannot cancel this hunter's operation.
    held_key = SDL_SCANCODE_X;
    sample(0, 0, UINT64_MAX, false, false);
    assert(!mix_armed && host_buttons == 0);
    active_weapon = 0;
    mix_armed = true;
    sample(0, 0, UINT64_MAX, false, false);
    assert(mix_armed);
    active_weapon = ACT_WEAPONS;
    held_key = SDL_SCANCODE_Z;
    sample(0, 0, UINT64_MAX, false, false);
    assert(!mix_armed);  // Invalid weapon indices safely use the common bindings.

    profile("[Actions]\nEnable=0\n");
    mix_armed = true;
    sample(0, B(BTN_X), UINT64_MAX, false, false);
    assert(host_buttons == (B(BTN_A) | B(BTN_X)));
    sample(0, B(BTN_B), UINT64_MAX, false, false);
    assert(!mix_armed && host_buttons == B(BTN_B));
    puts("PASS: auto combine honors remapped menu cancellation/navigation and the active weapon variant without replacing real inputs or action edges");
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

    // Hunter Art taps carry native combinations, including R+A+B and swapped shoulder roles.
    const uint64_t art_buttons[] = {0x2000,0x4000,0x8000,0x83,0x203,0x100,0x40,0x200,0x80,
                                    0x104,0x101,0x102,0x44,0x41,0x42};
    for (unsigned art = 0; art < sizeof art_buttons / sizeof *art_buttons; ++art) {
        uint32_t expected = 0;
        for (int btn = 0; btn < 16; ++btn)
            if (art_buttons[art] & (1ull << btn)) expected |= engine_bit[btn];
        sample(0, 0, UINT64_MAX, false, false);
        sample(art_buttons[art], 0, UINT64_MAX, false, false);
        assert(host_buttons == art_buttons[art]);
        for (int variant = 0; variant < ACT_VARS; ++variant)
            for (unsigned view = 0; view < sizeof views / sizeof *views; ++view)
                assert(act_view(views[view], expected, staged.hold[variant], false) == expected);
    }
    puts("PASS: Hunter Art direction/shoulder/face combinations survive real host action staging");

    const uint64_t r = 1ull << 7;  // BTN_R in the host's Switch button word.
    sample(0, r, UINT64_MAX, false, false);
    assert(host_buttons == r && !held_key);
    expect_r(false);  // A raw R bit without a physical or marked touch source is replaced by ActView.
    sample(r, 0, UINT64_MAX, false, false);
    expect_r(true);   // AutoR must return its native R in touch_b to survive the real host staging.
    held_key = SDL_SCANCODE_A + 7;
    sample(r, r, UINT64_MAX, false, false);
    expect_r(true);
    sample(0, r, UINT64_MAX, false, false);
    expect_r(true);   // Releasing touch leaves the physical R source held.
    sample(r, r, UINT64_MAX, false, false);
    held_key = 0;
    sample(r, 0, UINT64_MAX, false, false);
    expect_r(true);   // Releasing physical R leaves the marked touch source held.
    sample(0, 0, UINT64_MAX, false, false);
    expect_r(false);
    puts("PASS: real host/ActView preserves source-marked AutoR R, rejects unmarked raw R, and keeps either overlapping source through release");

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
    mix_auto_confirm();
    return 0;
}
