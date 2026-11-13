#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/nvn/vulkan/hud_layout.h"

static void near(float a, float b) { assert(fabsf(a - b) < 0.001f); }

int main(void) {
    const float aspects[] = {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f};
    for (unsigned i = 0; i < sizeof aspects / sizeof *aspects; i++) {
        float aspect = aspects[i], scale = (16.0f / 9.0f) / aspect;
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
    puts("HUD layout regressions: OK");
    return 0;
}
