#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/vulkan/hud_layout.h"
#include "hud_hunt_gate_under_test.h"
#include "hud_group_under_test.h"

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
        {1200, 95.1f, 100, HUD_TOUCH_NONE}, {1200, 50, 101, HUD_TOUCH_TARGET},
        {1000, 650, 103, HUD_TOUCH_ITEM}, {850, 500, 103, HUD_TOUCH_ITEM},
        // The status bars' right ends must not inherit map or item offsets.
        {800, 180, 103, HUD_TOUCH_NONE}, {1062, 33, 103, HUD_TOUCH_NONE},
        {899, 299.9f, 103, HUD_TOUCH_NONE}, {900, 299.9f, 103, HUD_TOUCH_NONE},
        {1062, 299.9f, 102.51f, HUD_TOUCH_NONE}, {1062, 299.9f, 103.49f, HUD_TOUCH_NONE},
        // Ammo is above the item bar, but remains part of the same movable group.
        {900, 300, 103, HUD_TOUCH_ITEM}, {899, 300, 103, HUD_TOUCH_MAP},
        {1010, 420, 103, HUD_TOUCH_ITEM}, {950, 305, 102.51f, HUD_TOUCH_ITEM},
        {900, 499, 103.49f, HUD_TOUCH_ITEM}, {899, 499, 103, HUD_TOUCH_NONE},
        {1000, 650, 102.5f, HUD_TOUCH_NONE}, {1000, 650, 103.5f, HUD_TOUCH_NONE},
        {800, 650, 103, HUD_TOUCH_ITEM}, {1000, 650, 100, HUD_TOUCH_NONE},
        {1150, 180, 105, HUD_TOUCH_MAP}, {980, 75, 104.51f, HUD_TOUCH_MAP},
        {1230, 310, 105.49f, HUD_TOUCH_MAP}, {1150, 180, 104.5f, HUD_TOUCH_MAP},
        {1150, 180, 105.5f, HUD_TOUCH_NONE},
        {800, 50, 100, HUD_TOUCH_MAP}, {800, 50, 101, HUD_TOUCH_MAP},
        {1200, 50, 101, HUD_TOUCH_TARGET},
        {400, 0, 104.5f, HUD_TOUCH_NONE}, {400, 0, 105, HUD_TOUCH_NONE},
        {1100, 360, 105.49f, HUD_TOUCH_MAP}, {399.9f, 200, 105, HUD_TOUCH_NONE},
        {1280.1f, 200, 105, HUD_TOUCH_NONE}, {800, 360.1f, 105, HUD_TOUCH_NONE},
        {800, 300, 103, HUD_TOUCH_MAP}, {950, 305, 102.51f, HUD_TOUCH_ITEM},
        {215, 0, 105, HUD_TOUCH_NONE}, // Meownster Hunters' map-frame sentinel.
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        int actual = hud_touch_zone(cases[i].x, cases[i].y, cases[i].z, true);
        if (actual != cases[i].zone) fprintf(stderr, "zone case %u at %.1f,%.1f z %.2f: got %d, expected %d\n", i, cases[i].x,
                                               cases[i].y, cases[i].z, actual, cases[i].zone);
        assert(actual == cases[i].zone);
        assert(hud_touch_zone(cases[i].x, cases[i].y, cases[i].z, false) == HUD_TOUCH_NONE);
    }
}

static void production_quest_gate_and_offsets(void) {
    const struct { float x, y, z; int zone; } pieces[] = {
        // Synthetic village-panel points overlap these zones only during a quest.
        {800, 180, 100, HUD_TOUCH_MAP}, {1000, 180, 101, HUD_TOUCH_MAP},
        {800, 650, 100, HUD_TOUCH_ARTS}, {1200, 50, 100, HUD_TOUCH_TARGET},
        {1000, 650, 103, HUD_TOUCH_ITEM}, {1010, 420, 103, HUD_TOUCH_ITEM},
        {1150, 180, 105, HUD_TOUCH_MAP}, {800, 300, 103, HUD_TOUCH_MAP},
        {200, 33, 103, HUD_TOUCH_NONE}, {800, 33, 103, HUD_TOUCH_NONE},
        {1062, 33, 103, HUD_TOUCH_NONE}, {1062, 299.9f, 103, HUD_TOUCH_NONE},
    };
    volatile float dx[HUD_TOUCH_COUNT] = {21, 42, 63, 84};
    volatile float dy[HUD_TOUCH_COUNT] = {-14, -28, -56, -70};
    for (unsigned flags = 0; flags < 256; ++flags) {
        bool quest = (flags & 1u) != 0;
        bool menu = (flags & 2u) != 0, results = (flags & 4u) != 0;
        bool results_last = (flags & 8u) != 0, loading = (flags & 16u) != 0;
        bool loading_candidate = (flags & 32u) != 0, title = (flags & 64u) != 0;
        bool meownster = (flags & 128u) != 0;
        bool hunt_hud = hud_test_hunt_gate(quest, menu, results, results_last, loading,
                                         loading_candidate, title, meownster);
        if (hunt_hud != (flags == 1u)) fprintf(stderr, "production hunt gate flags %u: got %d\n", flags, hunt_hud);
        assert(hunt_hud == (flags == 1u));
        for (unsigned i = 0; i < sizeof pieces / sizeof *pieces; ++i) {
            int expected = hunt_hud ? pieces[i].zone : HUD_TOUCH_NONE;
            int zone = hud_touch_zone_for_frame(pieces[i].x, pieces[i].y, pieces[i].z,
                                                hunt_hud, loading_candidate);
            assert(zone == expected);
            HudTouchOffset offset = hud_touch_offset(zone, true, dx, dy, 0.75f, 0.9f);
            assert(offset.active == (expected != HUD_TOUCH_NONE));
            assert(offset.moved == offset.active);
            near(offset.x, expected == HUD_TOUCH_NONE ? 0 : dx[expected] * 0.75f);
            near(offset.y, expected == HUD_TOUCH_NONE ? 0 : dy[expected] * 0.9f);
        }
    }
    puts("HUD production quest gate: 256 state combinations, panel/status exclusions and quest offsets OK");
}

static void reset_groups(void) {
    memset(&ui_group, 0, sizeof ui_group);
    memset(&ui_group_prev, 0, sizeof ui_group_prev);
    memset(ui_group_known, 0, sizeof ui_group_known);
    memset(ui_recent, 0, sizeof ui_recent);
    ui_nknown = ui_nrecent = 0;
}

static bool group_piece(float m[16], float x, float y, float z, bool menu, HudTouchOffset offset) {
    float s = fminf(1.0f, (16.0f / 9.0f) / g_render_aspect);
    float sy = fminf(1.0f, g_render_aspect / (16.0f / 9.0f));
    float left = 640.0f * (1.0f - g_ui_anchor_aspect / g_render_aspect);
    int anchor = hud_canvas_anchor(z, HUD_RIGHT, menu, false, false);
    m[12] = hud_map_x(x, s, left, 1280.0f - left, anchor) + offset.x;
    m[13] = y * sy + offset.y;
    ui_recent[ui_nrecent % UI_RECENT].hanchor = (uint8_t)anchor;
    ui_nrecent++;
    bool grouped = hud_test_grouped(s, sy, z, y, menu, false, false, offset);
    if (grouped) ui_group_add(m, z, x, y, 0);
    return grouped;
}

static void production_menu_groups(void) {
    const float aspects[] = {16.0f / 9.0f, 2.2f, 21.0f / 9.0f, 32.0f / 9.0f, 16.0f / 10.0f};
    const struct { const char *name; float aspect; } modes[] = {
        {"16:9 control", 16.0f / 9.0f}, {"21:9", 21.0f / 9.0f},
        {"32:9", 32.0f / 9.0f}, {"Edges", INFINITY},
    };
    // Body, text and isolated right border are independent groups on different layers.
    const struct { float x, y, z; } pieces[] = {
        {270, 80, 97}, {900, 80, 97}, {280, 100, 98},
        {700, 100, 98}, {930, 100, 100}, {930, 500, 100},
    };
    for (unsigned i = 0; i < sizeof aspects / sizeof *aspects; ++i) {
        for (unsigned mode = 0; mode < sizeof modes / sizeof *modes; ++mode) {
            g_render_aspect = aspects[i];
            g_ui_anchor_aspect = fminf(modes[mode].aspect, g_render_aspect);
            reset_groups();
            float matrices[sizeof pieces / sizeof *pieces][16] = {{0}};
            float resolved[sizeof pieces / sizeof *pieces];
            unsigned grouped = 0;
            for (unsigned p = 0; p < sizeof pieces / sizeof *pieces; ++p) {
                grouped += group_piece(matrices[p], pieces[p].x, pieces[p].y, pieces[p].z, true, (HudTouchOffset){0});
                resolved[p] = matrices[p][12];
            }
            ui_group_end();
            if (g_render_aspect < 16.0f / 9.0f) assert(grouped == sizeof pieces / sizeof *pieces);
            if (g_render_aspect == 16.0f / 9.0f) assert(grouped == 0);
            for (unsigned p = 0; p < sizeof pieces / sizeof *pieces; ++p) {
                if (fabsf(matrices[p][12] - resolved[p]) >= 0.001f)
                    fprintf(stderr, "menu group render %.4f HUD %s piece %u: x %.3f -> %.3f\n",
                            g_render_aspect, modes[mode].name, p, resolved[p], matrices[p][12]);
                near(matrices[p][12], resolved[p]);
                if (g_ui_anchor_aspect > 16.0f / 9.0f) assert(ui_recent[p].hanchor == HUD_LEFT);
            }
            near(matrices[4][12] - matrices[0][12], resolved[4] - resolved[0]);

            // Non-menu windows retain automatic grouping.
            reset_groups();
            float window[2][16] = {{0}};
            bool automatic = group_piece(window[0], 100, 80, 100, false, (HudTouchOffset){0});
            assert(group_piece(window[1], 300, 80, 100, false, (HudTouchOffset){0}) == automatic);
            ui_group_end();
            assert(automatic == (g_render_aspect != 16.0f / 9.0f));
            if (g_render_aspect > 16.0f / 9.0f) {
                assert(ui_recent[0].hanchor == HUD_LEFT && ui_recent[1].hanchor == HUD_LEFT);
                near(window[1][12] - window[0][12], 200.0f * (16.0f / 9.0f) / g_render_aspect);
            }

            // A deliberately customized quest Arts position stays out of grouping in every mode.
            reset_groups();
            volatile float dx[HUD_TOUCH_COUNT] = {0, 42, 0, 0}, dy[HUD_TOUCH_COUNT] = {0, -28, 0, 0};
            float s = fminf(1.0f, (16.0f / 9.0f) / g_render_aspect);
            float sy = fminf(1.0f, g_render_aspect / (16.0f / 9.0f));
            bool hunt = hud_test_hunt_gate(true, false, false, false, false, false, false, false);
            int zone = hud_touch_zone_for_frame(800, 650, 100, hunt, false);
            assert(zone == HUD_TOUCH_ARTS);
            HudTouchOffset offset = hud_touch_offset(zone, true, dx, dy, s, sy);
            float art[16] = {0};
            assert(!group_piece(art, 800, 650, 100, false, offset));
            float art_x = art[12], art_y = art[13];
            ui_group_end();
            near(art[12], art_x); near(art[13], art_y);
            assert(offset.active && offset.moved);
            near(offset.x, 42 * s); near(offset.y, -28 * sy);
        }
    }
    puts("HUD production final grouping: 5 render aspects x 4 HUD modes preserve menu X, non-menu grouping and custom Arts offsets");
}

static void movable_map_and_loading_gate(void) {
    const float layers[] = {99.5f, 100.0f, 101.0f, 102.0f, 102.5f, 104.5f, 105.0f, 105.49f, 105.5f};
    for (unsigned i = 0; i < sizeof layers / sizeof *layers; ++i) {
        int expected = layers[i] >= 99.5f && layers[i] < 105.5f ? HUD_TOUCH_MAP : HUD_TOUCH_NONE;
        assert(hud_touch_zone(800, 180, layers[i], true) == expected);
        assert(hud_touch_zone(1150, 180, layers[i], false) == HUD_TOUCH_NONE);
    }
    assert(hud_touch_zone_for_frame(800, 180, 105, true, true) == HUD_TOUCH_NONE);
    assert(hud_touch_zone_for_frame(800, 50, 101, true, true) == HUD_TOUCH_NONE);
    assert(hud_touch_zone_for_frame(800, 180, 105, false, false) == HUD_TOUCH_NONE);
    assert(hud_loading_candidate(1, 10, 0));
    assert(hud_loading_candidate(0, 10, 8));
    assert(!hud_loading_candidate(0, 11, 8));
    assert(!hud_loading_candidate(0, 7, 8));
    int horizontal = HUD_CENTER, vertical = HUD_V_BOTTOM;
    hud_touch_anchor(HUD_TOUCH_MAP, &horizontal, &vertical);
    assert(horizontal == HUD_RIGHT && vertical == HUD_V_TOP);
    horizontal = HUD_CENTER, vertical = HUD_V_BOTTOM;
    hud_touch_anchor(HUD_TOUCH_ITEM, &horizontal, &vertical);
    assert(horizontal == HUD_CENTER && vertical == HUD_V_BOTTOM);
    volatile float dx[HUD_TOUCH_COUNT] = {0, 0, 0, 36};
    volatile float dy[HUD_TOUCH_COUNT] = {0, 0, 0, -24};
    HudTouchOffset offset = hud_touch_offset(HUD_TOUCH_MAP, true, dx, dy, 0.5f, 0.75f);
    assert(offset.active && offset.moved); near(offset.x, 18); near(offset.y, -18);
    assert(!hud_touch_group_allowed(true, offset));
    assert(hud_touch_relax_scissor(false, offset));
    offset = hud_touch_offset(HUD_TOUCH_MAP, false, dx, dy, 1, 1);
    assert(!offset.active && !offset.moved); near(offset.x, 0); near(offset.y, 0);
    assert(hud_canvas_anchor(105, HUD_RIGHT, false, false, false) == HUD_RIGHT);
    assert(hud_canvas_anchor(105, HUD_RIGHT, true, false, false) == HUD_LEFT);
    puts("HUD movable map/loading exclusion regressions: OK");
}

static void touch_offsets(void) {
    const struct {
        const char *aspect;
        float sx, sy, x[HUD_TOUCH_COUNT], y[HUD_TOUCH_COUNT];
    } canvases[] = {
        {"16:9", 1, 1, {21, 42, 63, 84}, {-14, -28, -56, -70}},
        {"2.2", 80.0f / 99.0f, 1, {16.969697f, 33.939394f, 50.909091f, 67.878788f}, {-14, -28, -56, -70}},
        {"21:9", 16.0f / 21.0f, 1, {16, 32, 48, 64}, {-14, -28, -56, -70}},
        {"32:9", 0.5f, 1, {10.5f, 21, 31.5f, 42}, {-14, -28, -56, -70}},
        {"16:10", 1, 0.9f, {21, 42, 63, 84}, {-12.6f, -25.2f, -50.4f, -63}},
    };
    for (unsigned i = 0; i < sizeof canvases / sizeof *canvases; ++i) {
        for (int sign = -1; sign <= 1; sign += 2) {
            volatile float dx[HUD_TOUCH_COUNT] = {sign * 21.0f, sign * 42.0f, sign * 63.0f, sign * 84.0f};
            volatile float dy[HUD_TOUCH_COUNT] = {sign * -14.0f, sign * -28.0f, sign * -56.0f, sign * -70.0f};
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
    const float aspects[] = {16.0f / 9.0f, 2.2f, 21.0f / 9.0f, 32.0f / 9.0f, 16.0f / 10.0f};
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
    production_quest_gate_and_offsets();
    production_menu_groups();
    movable_map_and_loading_gate();
    touch_offsets();
    puts("HUD layout regressions: OK");
    return 0;
}
