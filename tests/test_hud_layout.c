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

static void hunter_art_rows(void) {
    HudArtRows rows = {0};
    float y;
    assert(!hud_art_rows_map(&rows, 600, &y));
    // Arbitrary native coordinates: slot three alone retains the third designed touch display position.
    hud_art_rows_piece(&rows, 785);
    hud_art_rows_slot(&rows, 780, 2);
    hud_art_rows_piece(&rows, 780);
    hud_art_rows_finish(&rows);
    assert(rows.nrows == 1 && rows.rows[0].identified);
    assert(hud_art_rows_map(&rows, 780, &y)); near(y, 173);
    assert(hud_art_rows_map(&rows, 785, &y)); near(y, 178);
    assert(!hud_art_rows_map(&rows, 900, &y));
    assert(!hud_art_rows_map(&rows, NAN, &y));
    assert(!hud_art_rows_map(&rows, 780, NULL));
    // A partial redraw keeps the previously established anchor/position, but supplies no new art identity.
    hud_art_rows_piece(&rows, 785);
    hud_art_rows_finish(&rows);
    assert(rows.nrows == 1 && !rows.rows[0].identified);
    assert(hud_art_rows_map(&rows, 785, &y)); near(y, 178);
    // The renderer retains prev over item-only frames, and owns expiry by clearing both objects.
    HudArtRows prev = rows;
    assert(hud_art_rows_map(&prev, 780, &y)); near(y, 173);
    rows = (HudArtRows){0};
    assert(!hud_art_rows_map(&rows, 780, &y));

    const float native_y[] = {590, 640, 690};
    const int slots[] = {2, 0, 1};
    for (int i = 2; i >= 0; i--) {
        hud_art_rows_slot(&rows, native_y[i], slots[i]);
        hud_art_rows_piece(&rows, native_y[i] + 3);
        hud_art_rows_piece(&rows, native_y[i]);
    }
    hud_art_rows_finish(&rows);
    assert(rows.nrows == 3);
    for (int i = 0; i < 3; i++) {
        assert(rows.rows[i].identified);
        assert(hud_art_rows_map(&rows, native_y[i] + 3, &y)); near(y, 41 + slots[i] * 66 + 3);
    }
    // Partial updates of different native rows cannot re-number or move the others.
    hud_art_rows_piece(&rows, 643);
    hud_art_rows_finish(&rows);
    assert(hud_art_rows_map(&rows, 643, &y)); near(y, 44);
    assert(hud_art_rows_map(&rows, 693, &y)); near(y, 110);
    hud_art_rows_piece(&rows, 693);
    hud_art_rows_finish(&rows);
    assert(hud_art_rows_map(&rows, 693, &y)); near(y, 110);
    assert(hud_art_rows_map(&rows, 593, &y)); near(y, 176);
    // A real new icon on the same native row replaces the old slot identity.
    rows = (HudArtRows){0};
    hud_art_rows_piece(&rows, 600); hud_art_rows_slot(&rows, 600, 2); hud_art_rows_finish(&rows);
    hud_art_rows_piece(&rows, 600); hud_art_rows_slot(&rows, 600, 0); hud_art_rows_finish(&rows);
    assert(rows.rows[0].identified && rows.rows[0].slot == 0);
    assert(hud_art_rows_map(&rows, 600, &y)); near(y, 41);

    // Unknown empty panels fill remaining display positions without becoming valid art identities.
    rows = (HudArtRows){0};
    for (int i = 0; i < 3; i++) hud_art_rows_piece(&rows, 600 + 50 * i);
    hud_art_rows_slot(&rows, 600, 2); hud_art_rows_finish(&rows);
    assert(rows.rows[0].identified && !rows.rows[1].identified && !rows.rows[2].identified);
    assert(hud_art_rows_map(&rows, 600, &y)); near(y, 173);
    assert(hud_art_rows_map(&rows, 650, &y)); near(y, 41);
    assert(hud_art_rows_map(&rows, 700, &y)); near(y, 107);
    // Scaling and saved offsets follow mapping exactly once, including a narrow picture.
    assert(hud_art_rows_map(&rows, 605, &y));
    volatile float dx[HUD_TOUCH_COUNT] = {0}, dy[HUD_TOUCH_COUNT] = {0, -28, 0};
    HudTouchOffset offset = hud_touch_offset(HUD_TOUCH_ARTS, true, dx, dy, 1, 0.9f);
    near(y * 0.9f + offset.y, (173 + 5 - 28) * 0.9f);

    rows = (HudArtRows){0};
    hud_art_rows_piece(&rows, 600); hud_art_rows_piece(&rows, 650);
    hud_art_rows_slot(&rows, 600, 0); hud_art_rows_slot(&rows, 600, 1);
    hud_art_rows_slot(&rows, NAN, 2); hud_art_rows_slot(&rows, 650, 9);
    hud_art_rows_piece(&rows, INFINITY);
    hud_art_rows_finish(&rows);
    assert(rows.nrows == 2 && !rows.rows[0].identified && !rows.rows[1].identified);
    assert(rows.rows[0].slot != rows.rows[1].slot);
    rows = (HudArtRows){0};
    hud_art_rows_piece(&rows, 600); hud_art_rows_slot(&rows, 600, 0); hud_art_rows_slot(&rows, 650, 0);
    hud_art_rows_finish(&rows);
    assert(rows.nrows == 1 && !rows.rows[0].identified);
    rows = (HudArtRows){0};
    for (int i = 0; i < 4; i++) hud_art_rows_piece(&rows, 600 + 50 * i);
    hud_art_rows_finish(&rows);
    assert(!hud_art_rows_map(&rows, 600, &y));
    puts("Hunter Arts display rows: sparse/reordered/partial/conflicting/manual-offset cases OK");
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
    hunter_art_rows();
    puts("HUD layout regressions: OK");
    return 0;
}
