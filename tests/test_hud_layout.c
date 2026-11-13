#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/nvn/vulkan/hud_layout.h"

static void near(float a, float b) { assert(fabsf(a - b) < 0.001f); }

static void touch_zones(void) {
    const struct { float x, y, z; int zone; } cases[] = {
        {700, 561, 100, HUD_TOUCH_ARTS}, {900, 719, 99.51f, HUD_TOUCH_ARTS},
        {800, 600, 100.49f, HUD_TOUCH_ARTS}, {699.9f, 600, 100, HUD_TOUCH_NONE},
        {900.1f, 600, 100, HUD_TOUCH_NONE}, {800, 560, 100, HUD_TOUCH_NONE},
        {800, 600, 99.5f, HUD_TOUCH_NONE}, {800, 600, 100.5f, HUD_TOUCH_NONE},
        {1170, 16, 100, HUD_TOUCH_TARGET}, {1230, 95, 100, HUD_TOUCH_TARGET},
        {1200, 50, 100.49f, HUD_TOUCH_TARGET}, {1169.9f, 50, 100, HUD_TOUCH_NONE},
        {1230.1f, 50, 100, HUD_TOUCH_NONE}, {1200, 15.9f, 100, HUD_TOUCH_NONE},
        {1200, 95.1f, 100, HUD_TOUCH_NONE}, {1200, 50, 101, HUD_TOUCH_NONE},
        {1000, 650, 103, HUD_TOUCH_ITEM}, {850, 500, 103, HUD_TOUCH_ITEM},
        // Ammo is above the item bar, but remains part of the same movable group.
        {1010, 420, 103, HUD_TOUCH_ITEM}, {950, 305, 102.51f, HUD_TOUCH_ITEM},
        {900, 499, 103.49f, HUD_TOUCH_ITEM}, {899, 499, 103, HUD_TOUCH_NONE},
        {1000, 650, 102.5f, HUD_TOUCH_NONE}, {1000, 650, 103.5f, HUD_TOUCH_NONE},
        {800, 650, 103, HUD_TOUCH_ITEM}, {1000, 650, 100, HUD_TOUCH_NONE},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        assert(hud_touch_zone(cases[i].x, cases[i].y, cases[i].z, true) == cases[i].zone);
        assert(hud_touch_zone(cases[i].x, cases[i].y, cases[i].z, false) == HUD_TOUCH_NONE);
    }
}

static void touch_offsets(void) {
    const struct {
        const char *aspect;
        float sx, sy, x[HUD_TOUCH_COUNT], y[HUD_TOUCH_COUNT];
    } canvases[] = {
        {"16:9", 1, 1, {21, 42, 63}, {-14, -28, -56}},
        {"21:9", 16.0f / 21.0f, 1, {16, 32, 48}, {-14, -28, -56}},
        {"32:9", 0.5f, 1, {10.5f, 21, 31.5f}, {-14, -28, -56}},
        {"16:10", 1, 0.9f, {21, 42, 63}, {-12.6f, -25.2f, -50.4f}},
    };
    for (unsigned i = 0; i < sizeof canvases / sizeof *canvases; ++i) {
        for (int sign = -1; sign <= 1; sign += 2) {
            volatile float dx[HUD_TOUCH_COUNT] = {sign * 21.0f, sign * 42.0f, sign * 63.0f};
            volatile float dy[HUD_TOUCH_COUNT] = {sign * -14.0f, sign * -28.0f, sign * -56.0f};
            for (int zone = 0; zone < HUD_TOUCH_COUNT; ++zone) {
                HudTouchOffset offset = hud_touch_offset(zone, true, dx, dy, canvases[i].sx, canvases[i].sy);
                assert(offset.active && offset.moved);
                near(offset.x, sign * canvases[i].x[zone]);
                near(offset.y, sign * canvases[i].y[zone]);
                assert(!hud_touch_group_allowed(true, offset));
                assert(!hud_touch_group_allowed(false, offset));
                assert(hud_touch_relax_scissor(false, offset));
                assert(hud_touch_relax_scissor(true, offset));
                offset = hud_touch_offset(zone, false, dx, dy, canvases[i].sx, canvases[i].sy);
                assert(!offset.active && !offset.moved);
                near(offset.x, 0); near(offset.y, 0);
                assert(hud_touch_group_allowed(true, offset));
                assert(!hud_touch_group_allowed(false, offset));
                assert(!hud_touch_relax_scissor(false, offset));
                assert(hud_touch_relax_scissor(true, offset));
            }
        }
        printf("HUD manual offsets at %s: positive/negative/hidden regions OK\n", canvases[i].aspect);
    }
    volatile float dx[HUD_TOUCH_COUNT] = {0}, dy[HUD_TOUCH_COUNT] = {0};
    for (int zone = 0; zone < HUD_TOUCH_COUNT; ++zone) {
        HudTouchOffset offset = hud_touch_offset(zone, true, dx, dy, 1, 1);
        assert(offset.active && !offset.moved);
        near(offset.x, 0); near(offset.y, 0);
        // Even zero manual offsets own the feedback base; grouping must not overwrite it.
        assert(!hud_touch_group_allowed(true, offset));
        assert(!hud_touch_group_allowed(false, offset));
        assert(!hud_touch_relax_scissor(false, offset));
        assert(hud_touch_relax_scissor(true, offset));
        dx[zone] = 8;
        offset = hud_touch_offset(zone, true, dx, dy, 0.5f, 2);
        assert(offset.active && offset.moved); near(offset.x, 4); near(offset.y, 0);
        offset = hud_touch_offset(zone, true, dx, dy, 0, 2);
        assert(offset.active && !offset.moved); near(offset.x, 0); near(offset.y, 0);
        dx[zone] = 0; dy[zone] = -5;
        offset = hud_touch_offset(zone, true, dx, dy, 0.5f, 2);
        assert(offset.active && offset.moved); near(offset.x, 0); near(offset.y, -10);
        offset = hud_touch_offset(zone, true, dx, dy, 0.5f, 0);
        assert(offset.active && !offset.moved); near(offset.x, 0); near(offset.y, 0);
        dy[zone] = 0;
    }
    const int invalid[] = {HUD_TOUCH_NONE, HUD_TOUCH_COUNT, 99};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        for (int visible = 0; visible <= 1; ++visible) {
            HudTouchOffset offset = hud_touch_offset(invalid[i], visible != 0, dx, dy, 1, 1);
            assert(!offset.active && !offset.moved); near(offset.x, 0); near(offset.y, 0);
            assert(hud_touch_group_allowed(true, offset));
            assert(!hud_touch_group_allowed(false, offset));
            assert(!hud_touch_relax_scissor(false, offset));
            assert(hud_touch_relax_scissor(true, offset));
        }
}

int main(void) {
    const float aspects[] = {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, 16.0f / 10.0f};
    for (unsigned i = 0; i < sizeof aspects / sizeof *aspects; i++) {
        float aspect = aspects[i], scale = fminf(1.0f, (16.0f / 9.0f) / aspect);
        // Center, 21:9, 32:9 and Edges anchors, clamped to the render canvas.
        const float anchors[] = {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, aspect};
        for (unsigned j = 0; j < sizeof anchors / sizeof *anchors; j++) {
            float left = 640.0f * (1.0f - fminf(aspect, anchors[j]) / aspect), right = 1280.0f - left;
            const float layers[] = {96.0f, 97.0f, 98.0f, 100.0f, 101.0f, 105.0f};
            for (unsigned k = 0; k < sizeof layers / sizeof *layers; k++) {
                // Screenshot submenu: its body/text and right border used opposite anchors at x 430.
                int a0 = hud_canvas_anchor(layers[k], HUD_LEFT, true, false, false);
                int a1 = hud_canvas_anchor(layers[k], HUD_RIGHT, true, false, false);
                float x0 = hud_map_x(270, scale, left, right, a0);
                float x1 = hud_map_x(450, scale, left, right, a1);
                near(x1 - x0, 180 * scale);
                near(hud_map_x(360, scale, left, right, a0), (x0 + x1) * 0.5f);
                // The same mapping applies to a clipped list and its preview frame.
                near(hud_map_x(450, scale, left, right, a0), x1);
                if (i == 0) { near(x0, 270); near(x1, 450); }
            }
            // An animated scope/lock reticle cannot jump when crossing generic anchor thresholds.
            const float points[] = {429, 431, 649, 651};
            for (unsigned k = 0; k < sizeof points / sizeof *points; k++) {
                float x = points[k];
                int legacy = x <= 430 ? HUD_LEFT : x >= 650 ? HUD_RIGHT : HUD_CENTER;
                int a = hud_canvas_anchor(110, legacy, false, true, false);
                assert(a == HUD_CENTER);
                near(hud_map_x(x, scale, left, right, a), 640 + (x - 640) * scale);
            }
            assert(hud_canvas_anchor(97, HUD_RIGHT, false, true, false) == HUD_CENTER);
        }
    }
    assert(hud_loading_row(-30, 168) == 1);
    assert(hud_loading_row(549, 433) == 2);
    assert(hud_loading_row(1128, 570) == 4);
    assert(hud_loading_row(640, 168) == 0);
    assert(hud_loading_row(640, 433) == 0);
    assert(hud_loading_row(549, 360) == 0);
    assert(hud_menu_panel_allowed(270, 270, false, false));
    assert(!hud_menu_panel_allowed(30, 547, false, false));
    assert(!hud_menu_panel_allowed(270, 270, true, false));
    assert(!hud_menu_panel_allowed(640, 53, false, true));
    touch_zones();
    touch_offsets();
    puts("HUD layout regressions: OK");
    return 0;
}
