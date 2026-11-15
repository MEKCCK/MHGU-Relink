#pragma once
#include <stdbool.h>
#include <math.h>

enum { HUD_LEFT, HUD_RIGHT, HUD_CENTER };

enum { HUD_TOUCH_NONE = -1, HUD_TOUCH_ITEM, HUD_TOUCH_ARTS, HUD_TOUCH_TARGET, HUD_TOUCH_MAP, HUD_TOUCH_COUNT };

// The entire item group includes the bowgun's ammo list above y 500.
static inline int hud_touch_zone(float x, float y, float z, bool hunt_hud) {
    if (!hunt_hud) return HUD_TOUCH_NONE;
    if (fabsf(z - 100.0f) < 0.5f) {
        if (x >= 700.0f && x <= 900.0f && y > 560.0f) return HUD_TOUCH_ARTS;
        if (x >= 1170.0f && x <= 1230.0f && y >= 16.0f && y <= 95.0f) return HUD_TOUCH_TARGET;
    } else if (fabsf(z - 103.0f) < 0.5f && !(y < 500.0f && x < 900.0f)) {
        return HUD_TOUCH_ITEM;
    } else if (fabsf(z - 105.0f) < 0.5f) {
        // The minimap uses the same HUD layer in quests and in the village / Hub.
        return HUD_TOUCH_MAP;
    }
    return HUD_TOUCH_NONE;
}

typedef struct HudTouchOffset {
    float x, y;
    bool active, moved;
} HudTouchOffset;

// Manual placement follows touch visibility, independently of automatic MoveHUD.
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

// A moved minimap keeps its local crop. A whole-pass scissor must keep covering the pass.
// The caller has already applied the same horizontal / vertical anchors as the map pieces.
static inline void hud_touch_map_scissor(int rect[4], HudTouchOffset offset, float width, float height) {
    if (!offset.moved || width <= 0.0f || height <= 0.0f) return;
    if (!(rect[0] > 0 || rect[1] > 0 || rect[2] < width || rect[3] < height)) return;
    rect[0] += (int)lroundf(offset.x * width / 1280.0f);
    rect[1] += (int)lroundf(offset.y * height / 720.0f);
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
