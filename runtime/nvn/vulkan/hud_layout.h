#pragma once
#include <stdbool.h>
#include <math.h>

enum { HUD_LEFT, HUD_RIGHT, HUD_CENTER };

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
