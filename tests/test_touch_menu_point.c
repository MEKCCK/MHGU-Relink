// Actual host menu-coordinate bridge, with logical/physical window and renderer-map spies.
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct SDL_Window { int unused; } SDL_Window;
static SDL_Window test_window;
static bool has_window, valid_size, mapped;
static int logical_w, logical_h, physical_w, physical_h, pixel_size_queries;
static float g_render_aspect, screen_map[4];
static SDL_Window *ui_window(void) { return has_window ? &test_window : NULL; }
static bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h) {
    assert(window == &test_window);
    *w = logical_w; *h = logical_h;
    return valid_size;
}
static bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    assert(window == &test_window); ++pixel_size_queries;
    *w = physical_w; *h = physical_h;
    return true;
}
static bool menuptr_screen_map(float out[4]) {
    if (mapped) memcpy(out, screen_map, sizeof screen_map);
    return mapped;
}
#include "host_touch_menu_point_under_test.h"

static void fixture(int width, int height, float aspect) {
    has_window = valid_size = true; mapped = false;
    logical_w = width; logical_h = height;
    physical_w = width * 3; physical_h = height * 3; pixel_size_queries = 0;
    g_render_aspect = aspect;
    screen_map[0] = screen_map[2] = 1; screen_map[1] = screen_map[3] = 0;
}
static void point(float u, float v, float expected_x, float expected_y) {
    float x = -12345, y = -23456;
    assert(host_input_menu_point(u, v, &x, &y));
    assert(isfinite(x) && isfinite(y));
    assert(fabsf(x - expected_x) < .002f && fabsf(y - expected_y) < .002f);
    assert(pixel_size_queries == 0);
}
static void rejected(float u, float v) {
    float x = 1234.5f, y = -3456.25f;
    assert(!host_input_menu_point(u, v, &x, &y));
    assert(x == 1234.5f && y == -3456.25f);
    assert(pixel_size_queries == 0);
}
static void ordinary_and_wide(void) {
    fixture(1280, 720, 16.f / 9);
    point(0, 0, 0, 0); point(.25f, .5f, 320, 360); point(.5f, .5f, 640, 360);
    point(nextafterf(1, 0), nextafterf(1, 0), nextafterf(1, 0) * 1280, nextafterf(1, 0) * 720);
    fixture(2520, 1080, 7.f / 3);
    point(300.f / 2520, 0, 0, 0);
    point(780.f / 2520, .25f, 320, 180);
    point(.5f, .5f, 640, 360);
    puts("PASS: 16:9 and phone-wide default menu placement preserve exact layout coordinates and edge pixels");
}
static void renderer_and_high_dpi(void) {
    fixture(2520, 1080, 7.f / 3); mapped = true;
    screen_map[0] = .6f; screen_map[1] = 100; screen_map[2] = .8f; screen_map[3] = 36;
    point(340.f / 1280, 196.f / 720, 400, 200);
    point(484.f / 1280, 324.f / 720, 640, 360);
    // Same normalized finger, different logical and physical sizes: no physical-pixel reinterpretation.
    logical_w = 1260; logical_h = 540; physical_w = 3780; physical_h = 1620;
    point(340.f / 1280, 196.f / 720, 400, 200);
    fixture(800, 450, 16.f / 9); physical_w = 3200; physical_h = 1800;
    point(.25f, .75f, 320, 540);
    puts("PASS: renderer translation/nonuniform scaling and high-DPI windows map normalized fingers through logical coordinates");
}
static void letterbox(void) {
    fixture(720, 1280, 16.f / 9);
    point(.25f, .5f, 320, 360);
    point(0, 437.5f / 1280, 0, 0);
    rejected(.5f, .2f); rejected(.5f, .9f);
    rejected(.5f, 842.5f / 1280);
    fixture(1024, 768, 16.f / 9);
    point(.25f, .125f, 320, 0); point(.5f, .5f, 640, 360);
    rejected(.5f, .124f); rejected(.5f, .875f);
    fixture(1920, 1080, 4.f / 3);
    point(.5f, .5f, 640, 360); point(.125f, .5f, 0, 360);
    rejected(.124f, .5f); rejected(.875f, .5f);
    puts("PASS: portrait, top/bottom and side letterboxes reject off-picture touches without changing outputs");
}
static void invalid_inputs(void) {
    fixture(1280, 720, 16.f / 9);
    for (int axis = 0; axis < 2; ++axis) {
        const float invalid[] = {-.001f, 1, 1.001f, NAN, INFINITY, -INFINITY};
        for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
            rejected(axis ? .5f : invalid[i], axis ? invalid[i] : .5f);
    }
    has_window = false; rejected(.5f, .5f); has_window = true;
    valid_size = false; rejected(.5f, .5f); valid_size = true;
    logical_w = 0; rejected(.5f, .5f); logical_w = -1; rejected(.5f, .5f); logical_w = 1280;
    logical_h = 0; rejected(.5f, .5f); logical_h = -1; rejected(.5f, .5f);
    fixture(1280, 720, 16.f / 9); mapped = true;
    for (int field = 0; field < 4; ++field) {
        for (int invalid = 0; invalid < 3; ++invalid) {
            screen_map[0] = screen_map[2] = 1; screen_map[1] = screen_map[3] = 0;
            screen_map[field] = invalid == 0 ? NAN : invalid == 1 ? INFINITY : -INFINITY;
            rejected(.5f, .5f);
        }
    }
    for (int field = 0; field <= 2; field += 2) for (int invalid = 0; invalid < 2; ++invalid) {
        screen_map[0] = screen_map[2] = 1; screen_map[1] = screen_map[3] = 0;
        screen_map[field] = invalid ? -1.f : 0.f;
        rejected(.5f, .5f);
    }
    puts("PASS: absent/invalid windows, invalid fingers and nonfinite/zero/negative renderer maps fail closed with unchanged outputs");
}
int main(void) {
    ordinary_and_wide(); renderer_and_high_dpi(); letterbox(); invalid_inputs();
    return 0;
}
