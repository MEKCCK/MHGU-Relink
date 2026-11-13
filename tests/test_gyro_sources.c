// The actual host gyro path with SDL virtual controllers. No physical sensor or game files are needed.
#include <assert.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "../runtime/config.h"
#include "../runtime/rt.h"
#include "../runtime/gyro.h"
#include "../runtime/hle/mousecam.h"
#include "../runtime/hle/chat.h"
#include "../runtime/textinput.h"
#include "../runtime/overlay.h"

static SDL_WindowFlags test_focus = SDL_WINDOW_INPUT_FOCUS;
static SDL_WindowFlags test_window_flags(SDL_Window *window) { (void)window; return test_focus; }
static SDL_Gamepad *pad;
static struct { SDL_Gamepad *pad; uint64_t opened_ns; bool real; } pads[8];
static int npads;
#define SDL_GetWindowFlags test_window_flags
// Generated verbatim from host_input.c by run_ranged_input.py, excluding unrelated input/script handlers.
#include "host_gyro_under_test.h"
#undef SDL_GetWindowFlags

static SDL_Window *test_window;
static bool test_aim, test_menu, test_chat, test_typing;
static int test_mode;
static float delivered_yaw, delivered_pitch;
SDL_Window *ui_window(void) { return test_window; }
int config_motion_mode(void) { return test_mode; }
int config_motion_flip_x(void) { return 0; }
int config_motion_flip_y(void) { return 0; }
uint32_t weapon_hunter(void) { return 1; }
bool weapon_aiming(uint32_t hunter) { return hunter && test_aim; }
bool overlay_menu_open(void) { return test_menu; }
bool chat_active(void) { return test_chat; }
bool textinput_active(void) { return test_typing; }
void mousecam_motion_add(float yaw, float pitch) { delivered_yaw += yaw; delivered_pitch += pitch; }
void mousecam_motion_reset(void) { delivered_yaw = delivered_pitch = 0; }

static bool SDLCALL enable_sensors(void *user, bool on) { (void)user; (void)on; return true; }
static SDL_JoystickID virtual_pad(const SDL_VirtualJoystickSensorDesc *sensors, int count) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes = 6; desc.nbuttons = 16;
    desc.name = "Ranged gyro regression";
    desc.nsensors = count; desc.sensors = sensors; desc.SetSensorsEnabled = enable_sensors;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc); assert(id);
    return id;
}
static void samples(SDL_JoystickID id, SDL_SensorType sensor, uint64_t base) {
    motion_sample(id, sensor, base, 1, 2);
    motion_sample(id, sensor, base + 10000000, 1, 2);
}
static void idle_gate(void) {
    motion_update();
    assert(!motion_active && !host_input_motion_phone_requested());
    for (int i = 0; i < npads; i++) {
        assert(!SDL_GamepadSensorEnabled(pads[i].pad, SDL_SENSOR_GYRO));
        assert(!SDL_GamepadSensorEnabled(pads[i].pad, SDL_SENSOR_GYRO_R));
        assert(!SDL_GamepadSensorEnabled(pads[i].pad, SDL_SENSOR_GYRO_L));
    }
    assert(delivered_yaw == 0 && delivered_pitch == 0);
}
int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR));
    test_window = SDL_CreateWindow("gyro regression", 64, 64, SDL_WINDOW_HIDDEN); assert(test_window);
    const SDL_VirtualJoystickSensorDesc all[] = {{SDL_SENSOR_GYRO, 100}, {SDL_SENSOR_GYRO_R, 100}, {SDL_SENSOR_GYRO_L, 100}};
    const SDL_VirtualJoystickSensorDesc split[] = {{SDL_SENSOR_GYRO_R, 100}, {SDL_SENSOR_GYRO_L, 100}};
    SDL_JoystickID ids[] = {virtual_pad(all, 3), virtual_pad(split, 2)};
    npads = 2;
    for (int i = 0; i < npads; i++) { pads[i].pad = SDL_OpenGamepad(ids[i]); assert(pads[i].pad); }
    pad = pads[0].pad;
    test_mode = MOTION_OFF; test_aim = true; idle_gate();
    test_mode = MOTION_AIM; test_aim = false; idle_gate();
    test_aim = true; assert(!motion_update());
    assert(SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO));
    assert(!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO_R));
    assert(!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO_L));
    assert(!host_input_motion_phone_requested());
    samples(0, SDL_SENSOR_GYRO, 1000000000); // phone cannot add to a selected controller
    samples(ids[1], SDL_SENSOR_GYRO_R, 1000000000); // inactive controller cannot turn the aim
    samples(ids[0], SDL_SENSOR_GYRO_L, 1000000000); // second gyro cannot double the turn
    assert(!motion_update());
    samples(ids[0], SDL_SENSOR_GYRO, 1000000000); assert(motion_update());
    assert(fabsf(delivered_yaw - .01f) < 1e-6f && fabsf(delivered_pitch - .02f) < 1e-6f);
    assert(!motion_update()); // last velocity is never extrapolated into another host update
    pad = pads[1].pad; assert(!motion_update());
    assert(delivered_yaw == 0); // source switch resets queued turns
    assert(!SDL_GamepadSensorEnabled(pads[0].pad, SDL_SENSOR_GYRO));
    assert(SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO_R));
    assert(!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO_L));
    samples(ids[1], SDL_SENSOR_GYRO_R, 2000000000); assert(motion_update());
    test_menu = true; idle_gate(); test_menu = false;
    test_chat = true; idle_gate(); test_chat = false;
    test_typing = true; idle_gate(); test_typing = false;
    test_focus = 0; idle_gate(); test_focus = SDL_WINDOW_INPUT_FOCUS;
    pad = NULL; assert(!motion_update()); assert(host_input_motion_phone_requested());
    samples(0, SDL_SENSOR_GYRO, 3000000000); assert(motion_update());
    test_aim = false; idle_gate();
    test_mode = MOTION_ALWAYS; idle_gate(); // legacy configuration also remains aiming only
    test_aim = true; assert(!motion_update());
    motion_sample(0, SDL_SENSOR_GYRO, 5000000000, 20, 20); assert(!motion_update()); // baseline on aim restart
    test_mode = MOTION_OFF; idle_gate();
    for (int i = 0; i < npads; i++) { SDL_CloseGamepad(pads[i].pad); assert(SDL_DetachVirtualJoystick(ids[i])); }
    SDL_DestroyWindow(test_window); SDL_Quit();
    puts("PASS: actual gyro source selection, one sensor at a time, phone fallback, aim/menu/focus gates, no replay or restart jump.");
    return 0;
}
