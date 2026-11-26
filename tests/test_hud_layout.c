#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/vulkan/hud_layout.h"
#include "hud_hunt_gate_under_test.h"
#include "hud_group_under_test.h"
#include "hud_loading_under_test.h"

static void near(float a, float b) { assert(fabsf(a - b) < 0.001f); }

// Synthetic, screenshot-derived positions, not captured guest matrices. The correct screenshot puts R 76.5
// canvas units left of B and A 37 left of B; B=709 uses the renderer's existing button-endpoint comment.
// The plus signs, panel endpoints and three row baselines complete the layout fixture.
static const struct { const char *name; float x; } art_combo[] = {
    {"R", 632.5f}, {"+", 652}, {"A", 672}, {"+", 691}, {"B", 709},
    {"panel left", 726}, {"panel right", 842},
};
static const float art_combo_y[] = {587, 631, 679};
enum { ART_COMBO_PIECES = sizeof art_combo / sizeof *art_combo,
       ART_COMBO_ROWS = sizeof art_combo_y / sizeof *art_combo_y };

static void touch_zones(void) {
    const struct { float x, y, z; int zone; } cases[] = {
        {630, 561, 100, HUD_TOUCH_ARTS}, {900, 719, 99.51f, HUD_TOUCH_ARTS},
        {800, 600, 100.49f, HUD_TOUCH_ARTS}, {629.9f, 600, 100, HUD_TOUCH_NONE},
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
        for (unsigned row = 0; row < ART_COMBO_ROWS; ++row) {
            for (unsigned p = 0; p < ART_COMBO_PIECES; ++p) {
                int expected = hunt_hud ? HUD_TOUCH_ARTS : HUD_TOUCH_NONE;
                int zone = hud_touch_zone_for_frame(art_combo[p].x, art_combo_y[row], 100,
                                                   hunt_hud, loading_candidate);
                if (zone != expected) fprintf(stderr, "combo gate flags %u row %u %s: got %d, expected %d\n",
                                              flags, row, art_combo[p].name, zone, expected);
                assert(zone == expected);
                HudTouchOffset offset = hud_touch_offset(zone, true, dx, dy, 0.75f, 0.9f);
                assert(offset.active == hunt_hud && offset.moved == hunt_hud);
                near(offset.x, hunt_hud ? 42 * 0.75f : 0);
                near(offset.y, hunt_hud ? -28 * 0.9f : 0);
            }
        }
    }
    puts("HUD production quest gate: 256 state combinations, complete synthetic R+A+B rows, panel/status exclusions and quest offsets OK");
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

static void production_art_combo_groups(void) {
    const float aspects[] = {16.0f / 9.0f, 2.2f, 21.0f / 9.0f, 32.0f / 9.0f, 16.0f / 10.0f};
    const float hud_aspects[] = {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, INFINITY};
    const struct { float x, y; } placements[] = {{0, 0}, {42, -28}, {-33, 19}, {0, -17}, {11, 0}};
    for (unsigned a = 0; a < sizeof aspects / sizeof *aspects; ++a) {
        g_render_aspect = aspects[a];
        float s = fminf(1.0f, (16.0f / 9.0f) / g_render_aspect);
        float sy = fminf(1.0f, g_render_aspect / (16.0f / 9.0f));
        for (unsigned h = 0; h < sizeof hud_aspects / sizeof *hud_aspects; ++h) {
            g_ui_anchor_aspect = fminf(hud_aspects[h], g_render_aspect);
            float base[ART_COMBO_ROWS][ART_COMBO_PIECES][2];
            for (unsigned placement = 0; placement < sizeof placements / sizeof *placements; ++placement) {
                reset_groups();
                // Keep an unrelated native group queued: flushing it must not rewrite any combo piece.
                float window[16] = {0};
                bool queued = group_piece(window, 100, 80, 100, false, (HudTouchOffset){0});
                assert(queued == (g_render_aspect != 16.0f / 9.0f));
                volatile float dx[HUD_TOUCH_COUNT] = {113, placements[placement].x, -71, 89};
                volatile float dy[HUD_TOUCH_COUNT] = {-97, placements[placement].y, 43, -61};
                float matrices[ART_COMBO_ROWS][ART_COMBO_PIECES][16] = {{{0}}};
                float resolved[ART_COMBO_ROWS][ART_COMBO_PIECES][2];
                for (unsigned row = 0; row < ART_COMBO_ROWS; ++row) {
                    for (unsigned p = 0; p < ART_COMBO_PIECES; ++p) {
                        int zone = hud_touch_zone_for_frame(art_combo[p].x, art_combo_y[row], 100, true, false);
                        if (zone != HUD_TOUCH_ARTS) fprintf(stderr, "combo row %u %s x %.1f: got zone %d\n",
                                                          row, art_combo[p].name, art_combo[p].x, zone);
                        assert(zone == HUD_TOUCH_ARTS);
                        HudTouchOffset offset = hud_touch_offset(zone, true, dx, dy, s, sy);
                        assert(offset.active);
                        assert(offset.moved == (placements[placement].x != 0 || placements[placement].y != 0));
                        near(offset.x, placements[placement].x * s);
                        near(offset.y, placements[placement].y * sy);
                        assert(!group_piece(matrices[row][p], art_combo[p].x, art_combo_y[row], 100, false, offset));
                        resolved[row][p][0] = matrices[row][p][12];
                        resolved[row][p][1] = matrices[row][p][13];
                    }
                }
                assert(ui_group.n == (queued ? 1 : 0));
                ui_group_end();
                for (unsigned row = 0; row < ART_COMBO_ROWS; ++row) {
                    for (unsigned p = 0; p < ART_COMBO_PIECES; ++p) {
                        near(matrices[row][p][12], resolved[row][p][0]);
                        near(matrices[row][p][13], resolved[row][p][1]);
                        if (!placement) {
                            base[row][p][0] = matrices[row][p][12];
                            base[row][p][1] = matrices[row][p][13];
                        }
                        near(matrices[row][p][12] - base[row][p][0], placements[placement].x * s);
                        near(matrices[row][p][13] - base[row][p][1], placements[placement].y * sy);
                        near(matrices[row][p][12] - matrices[row][0][12], (art_combo[p].x - art_combo[0].x) * s);
                        near(matrices[row][p][13], matrices[row][0][13]);
                        near(matrices[row][p][13] - matrices[0][p][13], (art_combo_y[row] - art_combo_y[0]) * sy);
                        if (p) assert(matrices[row][p - 1][12] < matrices[row][p][12]);
                    }
                    near(matrices[row][4][12] - matrices[row][0][12], 76.5f * s);
                    near(matrices[row][4][12] - matrices[row][2][12], 37 * s);
                    assert(matrices[row][4][12] < matrices[row][5][12]);
                }
            }
        }
    }
    puts("HUD synthetic screenshot-derived complete R+A+B: 3 rows x 5 aspects x 4 HUD anchors x 5 manual offsets retain spacing and stay left of their panels through native group flush");
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

// Synthetic geometry exercises the observed row/stride contract; no guest matrices were captured.
static void loading_matrix(float m[16], float x, float y, unsigned shape) {
    memset(m, 0, 16 * sizeof *m);
    m[0] = 431.0f + shape; m[1] = 0.25f; m[4] = -0.5f;
    m[5] = 137.0f + shape; m[8] = 0.125f; m[9] = -0.125f;
    m[10] = m[15] = 1;
    m[12] = x; m[13] = y; m[14] = 110;
}

static void loading_begin(float aspect, float anchor) {
    g_render_aspect = aspect;
    g_ui_anchor_aspect = anchor;
    ui_group.n = 0;
    hud_test_loading_reset();
    assert(!ui_loading_pass && !ui_loading_rows && !ui_loading_tiles.n && !ui_loading_tiles.overflow);
}

static void loading_draw(const float raw[16], float final[16], bool aiming) {
    float s = fminf(1.0f, (16.0f / 9.0f) / g_render_aspect);
    float left = 640.0f * (1.0f - fminf(g_render_aspect, g_ui_anchor_aspect) / g_render_aspect);
    int anchor = raw[12] <= 430 ? HUD_LEFT : raw[12] >= 650 ? HUD_RIGHT : HUD_CENTER;
    anchor = hud_test_canvas_anchor(s, raw[14], anchor, false, aiming, false);
    memcpy(final, raw, 16 * sizeof *raw);
    final[12] = hud_map_x(raw[12], s, left, 1280.0f - left, anchor);
    ui_loading_record(hud_loading_tile(raw, s), final);
}

static void loading_only_x_changed(const float raw[16], const float final[16], float x) {
    for (unsigned i = 0; i < 16; i++) near(final[i], i == 12 ? x : raw[i]);
}

static void production_loading_phases(void) {
    const float aspects[] = {2.2f, 21.0f / 9.0f, 32.0f / 9.0f};
    const float ys[] = {168, 433, 570};
    const float boundary[] = {-609.31f, -609.29f, -30.31f, -30.29f, -30.01f, -30, -29.99f, -29.71f, -29.69f, 548.99f, 549.01f};
    const unsigned orders[][9] = {{0, 1, 2, 3, 4, 5, 6, 7, 8}, {8, 7, 6, 5, 4, 3, 2, 1, 0}, {0, 3, 6, 2, 5, 8, 1, 4, 7}};
    for (unsigned a = 0; a < sizeof aspects / sizeof *aspects; a++) {
        float s = (16.0f / 9.0f) / aspects[a];
        const float anchors[] = {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, aspects[a]};
        for (unsigned h = 0; h < sizeof anchors / sizeof *anchors; h++) {
            float previous = 0;
            for (unsigned phase = 0; phase < 580 + sizeof boundary / sizeof *boundary; phase++) {
                float x = phase < 580 ? (float)phase - 31 : boundary[phase - 580];
                float raw[9][16], final[9][16];
                loading_begin(aspects[a], anchors[h]);
                g_present_count = 1000 + phase;
                for (unsigned i = 0; i < 9; i++) {
                    unsigned row = i / 3;
                    loading_matrix(raw[i], x + row * 37.25f + (i % 3) * 579, ys[row], row);
                }
                for (unsigned i = 0; i < 9; i++) {
                    unsigned k = orders[phase % 3][i];
                    loading_draw(raw[k], final[k], (i & 1) != 0);
                    near(final[k][12], 640 + (raw[k][12] - 640) * s);
                }
                assert(ui_loading_pass && ui_loading_rows == 7 && ui_loading_seen_frame == g_present_count);
                hud_test_loading_finish();
                for (unsigned i = 0; i < 9; i++) loading_only_x_changed(raw[i], final[i], raw[i][12] * s);
                for (unsigned row = 0; row < 3; row++) near(final[row * 3 + 1][12] - final[row * 3][12], 579 * s);
                if (phase && phase < 580) near(final[0][12] - previous, s);
                previous = final[0][12];
                assert(!ui_loading_tiles.n && !ui_loading_tiles.rows && !ui_loading_tiles.overflow);
                final[0][12] = 12345;
                hud_test_loading_finish();
                near(final[0][12], 12345);
            }
        }
    }
}

static void production_loading_rejection(void) {
    const float ys[] = {168, 433, 570};
    const unsigned columns[] = {0, 1, 4, 5, 8, 9};
    const float s = (16.0f / 9.0f) / 2.2f;
    // No pair, one paired row, duplicate X, wrong stride, mismatched shape, wrong Y/layer and nonfinite shape.
    for (unsigned scenario = 0; scenario < 14; scenario++) {
        float raw[4][16], final[4][16], baseline[4];
        loading_begin(2.2f, 2.2f);
        ui_loading_seen_frame = 77;
        g_present_count = 200;
        loading_matrix(raw[0], -42, ys[0], 0);
        loading_matrix(raw[1], 537, ys[0], 0);
        loading_matrix(raw[2], 81, ys[1], 1);
        loading_matrix(raw[3], 660, ys[1], 1);
        if (scenario == 0) raw[1][12] = 102, raw[3][12] = 330, raw[3][13] = ys[2];
        else if (scenario == 1) raw[3][13] = ys[2];
        else if (scenario == 2) raw[1][12] = raw[0][12], raw[3][12] = raw[2][12];
        else if (scenario == 3) raw[3][12] -= 1;
        else if (scenario == 4) raw[3][12] += 1;
        else if (scenario < 11) raw[3][columns[scenario - 5]] += 0.01f;
        else if (scenario == 11) raw[3][13] = 360;
        else if (scenario == 12) raw[3][14] = 109;
        else raw[3][5] = INFINITY;
        for (unsigned i = 0; i < 4; i++) {
            loading_draw(raw[i], final[i], false);
            baseline[i] = final[i][12];
        }
        assert(!ui_loading_pass && !ui_loading_rows && ui_loading_seen_frame == 77);
        assert(!hud_loading_candidate(ui_loading_rows, g_present_count, ui_loading_seen_frame));
        hud_test_loading_finish();
        for (unsigned i = 0; i < 4; i++) {
            near(final[i][12], baseline[i]);
            near(final[i][13], raw[i][13]);
        }
    }
    // Mixed markers keep their own centred matrix even after two real rows confirm.
    float raw[9][16], final[9][16];
    loading_begin(2.2f, 2.2f);
    ui_loading_seen_frame = 77;
    g_present_count = 201;
    for (unsigned i = 0; i < 4; i++) loading_matrix(raw[i], (i % 2) * 579.0f - 42 + (i / 2) * 81, ys[i / 2], i / 2);
    for (unsigned i = 4; i < 9; i++) loading_matrix(raw[i], 200 + i * 13, ys[i % 3], 9);
    // Same shape as the first row but not on its repeated columns.
    loading_matrix(raw[8], 71, ys[0], 0);
    for (unsigned i = 0; i < 9; i++) {
        loading_draw(raw[i], final[i], true);
        if (i < 3) assert(!ui_loading_pass && !ui_loading_rows && ui_loading_seen_frame == 77);
    }
    assert(ui_loading_pass && ui_loading_rows == 3 && ui_loading_seen_frame == 201);
    assert(hud_test_canvas_anchor(s, 110, HUD_RIGHT, true, true, true) == HUD_CENTER);
    hud_test_loading_finish();
    for (unsigned i = 0; i < 9; i++)
        loading_only_x_changed(raw[i], final[i], i < 4 ? raw[i][12] * s : 640 + (raw[i][12] - 640) * s);

    // Evidence and matrix pointers from an unfinished pass must not reach a new pass.
    loading_begin(2.2f, 2.2f);
    for (unsigned i = 0; i < 4; i++) loading_draw(raw[i], final[i], false);
    loading_begin(2.2f, 2.2f);
    hud_test_loading_finish();
    for (unsigned i = 0; i < 4; i++) near(final[i][12], 640 + (raw[i][12] - 640) * s);
}

static void production_loading_overflow_and_legacy(void) {
    const float s = (16.0f / 9.0f) / 2.2f;
    float raw[HUD_LOADING_MAX + 2][16], final[HUD_LOADING_MAX + 2][16];
    loading_begin(2.2f, 2.2f);
    for (unsigned i = 0; i < HUD_LOADING_MAX + 2; i++) {
        loading_matrix(raw[i], (i % 2) * 579.0f - 42, i % 4 < 2 ? 168 : 433, i / 4);
        loading_draw(raw[i], final[i], false);
    }
    assert(ui_loading_tiles.overflow && !ui_loading_tiles.n && !ui_loading_tiles.rows);
    hud_test_loading_finish();
    for (unsigned i = 0; i < HUD_LOADING_MAX + 2; i++)
        loading_only_x_changed(raw[i], final[i], 640 + (raw[i][12] - 640) * s);
    loading_begin(2.2f, 2.2f);
    assert(!ui_loading_tiles.overflow);

    const float aspects[] = {16.0f / 9.0f, 16.0f / 10.0f};
    for (unsigned i = 0; i < sizeof aspects / sizeof *aspects; i++) {
        loading_begin(aspects[i], aspects[i]);
        float sy = fminf(1, aspects[i] / (16.0f / 9.0f));
        loading_matrix(raw[0], -30, 168, 0);
        loading_draw(raw[0], final[0], false);
        assert(!ui_loading_tiles.n && !ui_loading_pass);
        assert(hud_test_legacy_loading(1, -30, 168, 110, false, &sy));
        assert(!ui_loading_pass && ui_loading_rows == 1);
        assert(hud_test_legacy_loading(1, 549, 433, 110, false, &sy));
        assert(ui_loading_pass && ui_loading_rows == 3);
        near(sy, 1);
        assert(hud_test_canvas_anchor(1, 110, HUD_RIGHT, false, false, true) == HUD_RIGHT);
        loading_begin(aspects[i], aspects[i]);
        sy = fminf(1, aspects[i] / (16.0f / 9.0f));
        assert(!hud_test_legacy_loading(1, -30, 168, 110, true, &sy));
        assert(!ui_loading_pass && !ui_loading_rows);
        near(sy, fminf(1, aspects[i] / (16.0f / 9.0f)));
    }
    // The fixed phase and aiming gate are confined to the original non-wide branch.
    loading_begin(2.2f, 2.2f);
    float sy = 1;
    assert(!hud_test_legacy_loading(s, -30, 168, 110, false, &sy));
    assert(!ui_loading_pass && !ui_loading_rows);
}

static void cinematic_bars(void) {
    float bar[16] = {0};
    bar[0] = 1280.0f, bar[5] = 64.0f, bar[12] = 640.0f, bar[13] = 0.0f, bar[14] = 100.0f;
    HudCinematicBar layout = hud_cinematic_bar(bar, 16.0f / 9.0f / 2.2f);
    assert(layout.matched && layout.relax_scissor && layout.horizontal_scale == 1.0f);
    bar[13] = 720.0f;
    layout = hud_cinematic_bar(bar, 16.0f / 9.0f / 2.2f);
    assert(layout.matched && layout.relax_scissor && layout.horizontal_scale == 1.0f);
    assert(hud_cinematic_bar(bar, 1.0f).matched && !hud_cinematic_bar(bar, 1.0f).relax_scissor);
    bar[13] = 0.0f;
    const unsigned rejects[] = {12, 13, 14, 0, 5, 1, 4};
    const float values[] = {500.0f, 12.0f, 103.0f, 1099.0f, 129.0f, 1.0f, -1.0f};
    for (unsigned i = 0; i < sizeof rejects / sizeof *rejects; i++) {
        float original = bar[rejects[i]];
        bar[rejects[i]] = values[i];
        layout = hud_cinematic_bar(bar, 16.0f / 9.0f / 2.2f);
        assert(!layout.matched && !layout.relax_scissor && layout.horizontal_scale < 1.0f);
        bar[rejects[i]] = original;
    }
}

static void art_rows_off_canvas(void) {
    // The item box leaves the game with three Hunter Arts slots built, two empty and laid out below its 720-line
    // canvas (measured: y 664..696 on it, 731..746 and 781..796 below it). Moving the panel to the top right must
    // carry only the rows the game had on the canvas: the others stay below it, where the game put them.
    assert(hud_art_row_on_canvas(0) && hud_art_row_on_canvas(696) && hud_art_row_on_canvas(720));
    assert(!hud_art_row_on_canvas(720.5f) && !hud_art_row_on_canvas(731) && !hud_art_row_on_canvas(781));
    assert(!hud_art_row_on_canvas(-1));
    const float rows[] = {664, 681, 696, 731, 746, 781, 796};
    const float systems[] = {1.0f, 0.9f};  // (16:9, and 16:10's flatter canvas)
    for (unsigned i = 0; i < sizeof systems / sizeof *systems; i++) {
        float sy = systems[i], arts_top = 664;  // (the topmost row on the canvas: the panel's first line)
        for (unsigned k = 0; k < sizeof rows / sizeof *rows; k++) {
            float y = rows[k];
            float moved = hud_art_row_on_canvas(y) ? (y - arts_top + 41.0f) * sy  // (exec.c: ARTS_TOP)
                                                   : 720.0f + (y - 720.0f) * sy;
            if (hud_art_row_on_canvas(y)) assert(moved >= 0.0f && moved <= 720.0f);
            else assert(moved > 720.0f);  // (still off the canvas: never a slot beside the equipped Arts)
        }
        near((696.0f - arts_top + 41.0f) * sy, sy == 1.0f ? 73.0f : 65.7f);
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
    production_art_combo_groups();
    production_menu_groups();
    movable_map_and_loading_gate();
    touch_offsets();
    production_loading_phases();
    production_loading_rejection();
    production_loading_overflow_and_legacy();
    art_rows_off_canvas();
    cinematic_bars();
    puts("HUD layout regressions: OK");
    return 0;
}
