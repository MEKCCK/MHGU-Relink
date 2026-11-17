// Production mouse_update and host touch routing; platform services below are spies.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SDL_STATIC_LIB
#include <SDL3/SDL.h>

#include "config.h"
#include "hle/input_view.h"
#include "hle/menuptr.h"
#include "hle/mousecam.h"
#include "touch.h"

static struct {
    uint64_t now, buttons;
    float dx, dy, camera_x, camera_y;
    int left_x, left_y, right_x, right_y, camera_calls, relative_calls, resets;
    int capture;
    bool camera_active, overlay, chat, free_mouse, menu, hunt_menu;
    bool capture_succeeds, relative_wanted, lock_manual;
} spy;

#define LOG(...) ((void)0)
static uint64_t plat_now_ns(void) { return spy.now; }
static SDL_Window *ui_window(void) { return (SDL_Window *)(uintptr_t)1; }
static bool overlay_menu_open(void) { return spy.overlay; }
static bool chat_active(void) { return spy.chat; }
bool host_input_mouse_free(void) { return spy.free_mouse; }
bool mousecam_active(void) { return spy.camera_active; }
void mousecam_add(float dx, float dy) {
    ++spy.camera_calls;
    spy.camera_x += dx;
    spy.camera_y += dy;
}
int config_live_mouse_capture(void) { return spy.capture; }
float config_live_mouse_sensitivity(void) { return 1.0f; }
bool input_view_hunt_menu_open(void) { return spy.hunt_menu; }
void input_view_hunt_menu_reset(void) { ++spy.resets; }
bool menuptr_open(void) { return spy.menu; }
int menuptr_row(void) { return 0; }
void menuptr_row_centre(int row, float *x, float *y) { (void)row; *x = *y = 0; }
bool menuptr_geometry(MenuGeom *out) { (void)out; return false; }
static bool menu_ptr_geometry(SDL_Window *window, float *left, float *top, float *sx, float *sy) {
    (void)window; (void)left; (void)top; (void)sx; (void)sy;
    return false;
}
static bool real_pad_connected(void) { return false; }
uint64_t touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, int *rx, int *ry,
                     float *dx, float *dy) {
    assert(!pad_connected && !typing);
    *buttons |= spy.buttons;
    *lx += spy.left_x; *ly += spy.left_y;
    *rx += spy.right_x; *ry += spy.right_y;
    *dx = spy.dx; *dy = spy.dy;
    return spy.buttons;
}
uint64_t touch_lock_buttons(uint64_t buttons, bool manual) {
    (void)buttons;
    spy.lock_manual = manual;
    return 0;
}
bool SDLCALL SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled) {
    assert(window == ui_window());
    ++spy.relative_calls;
    spy.relative_wanted = enabled;
    return spy.capture_succeeds;
}
const char *SDLCALL SDL_GetError(void) { return "host capture refusal"; }
void SDLCALL SDL_WarpMouseInWindow(SDL_Window *window, float x, float y) { (void)window; (void)x; (void)y; }
bool SDLCALL SDL_ShowCursor(void) { return true; }
bool SDLCALL SDL_HideCursor(void) { return true; }
SDL_WindowFlags SDLCALL SDL_GetWindowFlags(SDL_Window *window) { (void)window; return 0; }
void SDLCALL SDL_LockSpinlock(SDL_SpinLock *lock) { (void)lock; }
void SDLCALL SDL_UnlockSpinlock(SDL_SpinLock *lock) { (void)lock; }

#include "host_touch_camera_under_test.h"

typedef struct { uint64_t buttons; int lx, ly, rx, ry; bool moved; } Input;
static Input sample(uint64_t buttons, int lx, int ly, int rx, int ry) {
    spy.now += 16000000ull;
    Input out = {buttons, lx, ly, rx, ry, false};
    out.moved = host_touch_camera_sample(false, &out.buttons, &out.lx, &out.ly, &out.rx, &out.ry);
    return out;
}
static void clear_counts(void) {
    spy.camera_calls = spy.relative_calls = 0;
    spy.camera_x = spy.camera_y = 0;
    spy.lock_manual = false;
}
static void reset(void) {
    uint64_t now = spy.now + 1000000000ull;
    memset(&spy, 0, sizeof spy);
    spy.now = now;
    spy.capture_succeeds = true;
    spy.capture = MOUSE_CAPTURE_CLICK;
    mouse_grab = mouse_captured = mouse_no_capture = mouse_released = false;
    mouse_dx = mouse_dy = test_vx = test_vy = 0;
    test_until = 0;
    int rx = 0, ry = 0;
    spy.camera_active = true;
    mouse_update(&rx, &ry);  // Clear the production smoother's function-local state.
    spy.camera_active = false;
    spy.now += 1000000000ull;
    mouse_update(&rx, &ry);
    clear_counts();
}
static void camera(float x, float y, int calls) {
    assert(spy.camera_calls == calls);
    assert(fabsf(spy.camera_x - x) < .0001f && fabsf(spy.camera_y - y) < .0001f);
}
static void touch_capture_transitions(void) {
    for (int mode = 0; mode < 3; ++mode) {
        reset();
        spy.camera_active = mode != 0;
        spy.capture = mode == 2 ? MOUSE_CAPTURE_HUNT : MOUSE_CAPTURE_CLICK;
        mouse_grab = mode != 2;
        spy.dx = 18; spy.dy = -11;
        mouse_dx = 300; mouse_dy = -900;  // Native capture intentionally discards the physical mouse's pending counts.
        Input out = sample(0, 0, 0, 1700, -2600);
        assert(mouse_captured && spy.relative_calls == 1 && spy.relative_wanted);
        assert(out.rx == 1700 && out.ry == -2600 && out.moved && spy.lock_manual);
        camera(18, -11, 1);

        clear_counts();
        mouse_grab = false;
        spy.camera_active = false;
        spy.dx = -13; spy.dy = 9;
        mouse_dx = 700; mouse_dy = 600;
        out = sample(0, 0, 0, 1700, -2600);
        assert(!mouse_captured && spy.relative_calls == 1 && !spy.relative_wanted);
        assert(out.rx == 1700 && out.ry == -2600 && out.moved && spy.lock_manual);
        camera(-13, 9, 1);

        spy.dx = spy.dy = 0;
        clear_counts();
        for (int frame = 0; frame < 20; ++frame) {
            out = sample(0, 0, 0, 0, 0);
            assert(!out.rx && !out.ry && !out.moved && !spy.lock_manual);
        }
        camera(0, 0, 0);
    }
    reset();
    mouse_grab = true;
    spy.capture_succeeds = false;
    spy.dx = 7; spy.dy = 3;
    Input out = sample(0, 0, 0, 0, 0);
    assert(mouse_no_capture && !mouse_captured && !out.rx && !out.ry);
    assert(out.moved && spy.lock_manual);
    camera(7, 3, 1);
    puts("PASS: touch camera counts survive click/hunt capture, release and capture refusal; release leaves no right-stick tail");
}
static void touch_preserves_native_axes(void) {
    for (int active = 0; active < 2; ++active) {
        reset();
        spy.camera_active = active != 0;
        spy.buttons = 0x104;  // Existing Hunter Art shoulder/face output remains native.
        spy.left_x = 400; spy.left_y = -600;
        spy.right_x = 11000; spy.right_y = 3000;
        spy.dx = 23; spy.dy = -14;
        Input out = sample(0x80, 1200, -2300, 1700, -2600);
        assert(out.buttons == 0x184 && out.lx == 1600 && out.ly == -2900);
        assert(out.rx == 12700 && out.ry == 400 && out.moved && spy.lock_manual);
        camera(23, -14, 1);
        spy.dx = spy.dy = 0;
        clear_counts();
        out = sample(0x80, 1200, -2300, 1700, -2600);
        assert(out.buttons == 0x184 && out.lx == 1600 && out.ly == -2900);
        assert(out.rx == 12700 && out.ry == 400 && !out.moved);
        camera(0, 0, 0);
    }
    puts("PASS: touch look uses camera angles with mousecam active or inactive and preserves Hunter Art axes, physical sticks and buttons");
}
static void physical_mouse_paths(void) {
    reset();
    spy.camera_active = true;
    mouse_grab = mouse_captured = true;
    mouse_dx = 12; mouse_dy = -9;
    Input out = sample(0, 0, 0, 1700, -2600);
    assert(out.rx == 1700 && out.ry == -2600 && out.moved && spy.lock_manual);
    camera(12, -9, 1);

    reset();
    mouse_grab = mouse_captured = true;
    mouse_dx = 30; mouse_dy = 10;
    out = sample(0, 0, 0, 0, 0);
    assert(out.rx > 0 && out.ry < 0 && out.moved && spy.lock_manual);
    camera(0, 0, 0);
    out = sample(0, 0, 0, 0, 0);
    assert(out.rx > 0 && out.ry < 0);  // The captured physical mouse retains its existing smoothing.
    mouse_grab = false;
    out = sample(0, 0, 0, 0, 0);
    assert(!mouse_captured && !out.rx && !out.ry && spy.relative_calls == 1 && !spy.relative_wanted);
    for (int frame = 0; frame < 20; ++frame) {
        out = sample(0, 0, 0, 0, 0);
        assert(!out.rx && !out.ry && !out.moved && !spy.lock_manual);
    }
    puts("PASS: physical mouse direct-camera and captured right-stick fallback retain manual-look marking and stop immediately on release");
}
int main(void) {
    touch_capture_transitions();
    touch_preserves_native_axes();
    physical_mouse_paths();
    return 0;
}
