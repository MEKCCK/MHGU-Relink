#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

enum { HUD_LEFT, HUD_RIGHT, HUD_CENTER };
enum { HUD_V_TOP, HUD_V_BOTTOM, HUD_V_CENTER };

enum { HUD_TOUCH_NONE = -1, HUD_TOUCH_ITEM, HUD_TOUCH_ARTS, HUD_TOUCH_TARGET, HUD_TOUCH_MAP, HUD_TOUCH_COUNT };

static inline bool hud_touch_map_piece(float x, float y, float z) {
    bool ammo_layer = fabsf(z - 103.0f) < 0.5f && x >= 900.0f;
    bool target_controls = z >= 99.5f && z < 101.5f && x >= 1160.0f && x <= 1240.0f && y <= 105.0f;
    return !ammo_layer && !target_controls && z >= 99.5f && z < 105.5f && x >= 740.0f && x <= 1280.0f && y >= 0.0f && y <= 360.0f;
}

static inline bool hud_touch_target_piece(float x, float y, float z) {
    return z >= 99.5f && z < 101.5f && x >= 1170.0f && x <= 1230.0f && y >= 16.0f && y <= 95.0f;
}

static inline bool hud_loading_candidate(unsigned rows, uint64_t frame, uint64_t seen_frame) {
    return rows != 0 || (seen_frame && frame >= seen_frame && frame - seen_frame <= 2);
}

static inline bool hud_art_row_on_canvas(float game_y) { return game_y >= 0.0f && game_y <= 720.0f; }

// [Display] HUDSize: the whole hunt HUD smaller, every part toward the corner it is laid out from (nvn/vulkan/exec.c).
// A part stays pinned at that corner of the game's 1280x720 canvas and its pieces shrink toward it. With the touch
// controls' MoveHUD the item bar, the Hunter Arts and the target camera are moved first, and their corner follows.
enum { HUD_SIZE_PART_STATUS, HUD_SIZE_PART_ITEM, HUD_SIZE_PART_ARTS, HUD_SIZE_PART_TARGET, HUD_SIZE_PART_MAP };

static inline void hud_size_anchor(int part, bool moved, float anchor_left, float anchor_right, float *x, float *y) {
    switch (part) {
    case HUD_SIZE_PART_ITEM: *x = moved ? 640.0f : anchor_right, *y = 720.0f; break;
    case HUD_SIZE_PART_ARTS: *x = moved ? 1280.0f : anchor_right, *y = moved ? 0.0f : 720.0f; break;
    case HUD_SIZE_PART_TARGET: *x = 1280.0f, *y = 0.0f; break;
    case HUD_SIZE_PART_MAP: *x = anchor_right, *y = 0.0f; break;
    default: *x = anchor_left, *y = 0.0f; break;  // the status HUD: the clock, the bars, the gauges, the party
    }
}

// A place after the HUD size: pinned to the part's corner, its distance from it scaled by that size (0.6 .. 1.0).
// A manual offset is added after this, so a part moved to the left edge stays that far from it, only smaller.
static inline float hud_size_place(float v, float corner, float size) { return corner + (v - corner) * size; }

// The entire item group includes the bowgun's ammo list above y 500.
static inline int hud_touch_zone_for_frame(float x, float y, float z, bool hunt_hud, bool loading_candidate) {
    if (!hunt_hud) return HUD_TOUCH_NONE;
    // HP/stamina span the top at any x; their ends belong to neither the map nor the item bar.
    if (fabsf(z - 103.0f) < 0.5f && y < 300.0f) return HUD_TOUCH_NONE;
    if (fabsf(z - 100.0f) < 0.5f) {
        // Type 2's R+A+B prefix extends left of the single-button prompts near x 709.
        if (x >= 630.0f && x <= 900.0f && y > 560.0f) return HUD_TOUCH_ARTS;
    }
    if (hud_touch_target_piece(x, y, z)) return HUD_TOUCH_TARGET;
    if (!loading_candidate && hud_touch_map_piece(x, y, z)) return HUD_TOUCH_MAP;
    if (fabsf(z - 103.0f) < 0.5f && !(y < 500.0f && x < 900.0f)) {
        return HUD_TOUCH_ITEM;
    }
    return HUD_TOUCH_NONE;
}

static inline int hud_touch_zone(float x, float y, float z, bool hunt_hud) {
    return hud_touch_zone_for_frame(x, y, z, hunt_hud, false);
}

static inline void hud_touch_anchor(int zone, int *horizontal, int *vertical) {
    if (zone == HUD_TOUCH_MAP) {
        *horizontal = HUD_RIGHT;
        *vertical = HUD_V_TOP;
    }
}

/*
 * Manual placement follows touch visibility, independently of automatic MoveHUD.
 */
typedef struct HudTouchOffset {
    float x, y;
    bool active, moved;
} HudTouchOffset;

static inline HudTouchOffset hud_touch_offset(int zone, bool visible, const volatile float dx[HUD_TOUCH_COUNT],
                                              const volatile float dy[HUD_TOUCH_COUNT], float sx, float sy) {
    HudTouchOffset offset = {0};
    offset.active = visible && zone >= 0 && zone < HUD_TOUCH_COUNT;
    if (offset.active) {
        offset.x = dx[zone] * sx;
        offset.y = dy[zone] * sy;
        offset.moved = offset.x != 0.0f || offset.y != 0.0f;
    }
    return offset;
}

static inline bool hud_touch_relax_scissor(bool relaxed, HudTouchOffset offset) {
    return relaxed || offset.moved;
}

// Group re-anchoring runs later and would overwrite both manual Y and its feedback base.
static inline bool hud_touch_group_allowed(bool grouped, HudTouchOffset offset) {
    return grouped && !offset.active;
}

static inline bool hud_menu_panel_allowed(float x, float y, bool aiming, bool stamp) {
    return !aiming && !stamp && !(x < 620.0f && y >= 540.0f);
}

static inline float hud_map_x(float x, float scale, float left, float right, int anchor) {
    return anchor == HUD_LEFT ? left + x * scale
         : anchor == HUD_RIGHT ? right - (1280.0f - x) * scale
         : 640.0f + (x - 640.0f) * scale;
}

typedef struct HudCinematicBar {
    bool matched, relax_scissor;
    float horizontal_scale;
} HudCinematicBar;

static inline HudCinematicBar hud_cinematic_bar(const float m[16], float scale) {
    bool matched = isfinite(m[0]) && fabsf(m[14] - 100.0f) < 0.5f && fabsf(m[12] - 640.0f) < 0.5f &&
                   (fabsf(m[13]) < 1.0f || fabsf(m[13] - 720.0f) < 1.0f) && fabsf(m[0]) >= 1100.0f &&
                   fabsf(m[5]) <= 128.0f && fabsf(m[1]) < 1.0f && fabsf(m[4]) < 1.0f;
    bool wide = matched && scale < 1.0f;
    return (HudCinematicBar){matched, wide, wide ? 1.0f : scale};
}

static inline int hud_canvas_anchor(float z, int anchor, bool menu, bool aiming, bool loading) {
    if (fabsf(z - 110.0f) < 0.5f && !loading) return HUD_CENTER;
    if (aiming && z > 96.5f && z < 98.5f) return HUD_CENTER;
    return menu ? HUD_LEFT : anchor;
}

static inline unsigned hud_loading_row(float x, float y) {
    // Reticles share this layer and can cross a row; loading tiles also have a fixed column stride.
    if (!(fabsf(remainderf(x + 30.0f, 579.0f)) < 0.3f)) return 0;
    return fabsf(y - 168.0f) < 0.3f ? 1u : fabsf(y - 433.0f) < 0.3f ? 2u
         : fabsf(y - 570.0f) < 0.3f ? 4u : 0u;
}

// A scrolling row has no fixed X phase; require matching neighbours in two rows before moving any tile.
#define HUD_LOADING_MAX 64
typedef struct HudLoadingTile {
    float *m, x, shape[6];
    unsigned row;
    bool paired;
} HudLoadingTile;

typedef struct HudLoadingPass {
    unsigned n, rows;
    bool overflow;
    HudLoadingTile p[HUD_LOADING_MAX];
} HudLoadingPass;

static inline HudLoadingTile hud_loading_tile(const float m[16], float scale) {
    HudLoadingTile tile = {0};
    if (!(scale < 1.0f) || !(fabsf(m[14] - 110.0f) < 0.5f) || !isfinite(m[12])) return tile;
    tile.row = fabsf(m[13] - 168.0f) < 0.3f ? 1u : fabsf(m[13] - 433.0f) < 0.3f ? 2u
             : fabsf(m[13] - 570.0f) < 0.3f ? 4u : 0u;
    tile.x = m[12];
    const unsigned columns[] = {0, 1, 4, 5, 8, 9};
    for (unsigned i = 0; i < 6; i++) {
        tile.shape[i] = m[columns[i]];
        if (!isfinite(tile.shape[i])) tile.row = 0;
    }
    return tile;
}

static inline bool hud_loading_add(HudLoadingPass *pass, HudLoadingTile tile, float *m) {
    if (!tile.row || pass->overflow) return false;
    if (pass->n == HUD_LOADING_MAX) {
        *pass = (HudLoadingPass){.overflow = true};
        return false;
    }
    tile.m = m;
    for (unsigned i = 0; i < pass->n; i++) {
        HudLoadingTile *other = &pass->p[i];
        if (other->row != tile.row || !(fabsf(fabsf(other->x - tile.x) - 579.0f) < 0.3f)) continue;
        bool same = true;
        for (unsigned k = 0; k < 6; k++) same &= other->shape[k] == tile.shape[k];
        if (same) {
            other->paired = tile.paired = true;
            pass->rows |= tile.row;
        }
    }
    pass->p[pass->n++] = tile;
    return (pass->rows & (pass->rows - 1)) != 0;
}

static inline void hud_loading_reset(HudLoadingPass *pass) {
    *pass = (HudLoadingPass){0};
}

static inline void hud_loading_finish(HudLoadingPass *pass, float scale) {
    if (scale < 1.0f && !pass->overflow && (pass->rows & (pass->rows - 1)))
        for (unsigned i = 0; i < pass->n; i++)
            if (pass->p[i].paired) pass->p[i].m[12] = pass->p[i].x * scale;
    hud_loading_reset(pass);  // The transient matrices are submitted when this pass ends.
}
