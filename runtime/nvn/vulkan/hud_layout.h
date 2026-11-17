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

// The entire item group includes the bowgun's ammo list above y 500.
static inline int hud_touch_zone_for_frame(float x, float y, float z, bool hunt_hud, bool loading_candidate) {
    if (!hunt_hud) return HUD_TOUCH_NONE;
    // HP/stamina span the top at any x; their ends belong to neither the map nor the item bar.
    if (fabsf(z - 103.0f) < 0.5f && y < 300.0f) return HUD_TOUCH_NONE;
    if (fabsf(z - 100.0f) < 0.5f) {
        if (x >= 700.0f && x <= 900.0f && y > 560.0f) return HUD_TOUCH_ARTS;
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
