#pragma once
#include <stdbool.h>
#include <math.h>

enum { HUD_LEFT, HUD_RIGHT, HUD_CENTER };

enum { HUD_TOUCH_NONE = -1, HUD_TOUCH_ITEM, HUD_TOUCH_ARTS, HUD_TOUCH_TARGET, HUD_TOUCH_COUNT };

typedef struct HudArtRow {
    float y0, y1, anchor;
    int slot;
    bool identified;
} HudArtRow;

typedef struct HudArtRows {
    float pieces[32], marker_y[3];
    int npieces, nrows;
    unsigned marker_mask, bad_markers;
    HudArtRow rows[3];
} HudArtRows;

static inline void hud_art_rows_piece(HudArtRows *rows, float gy) {
    if (isfinite(gy) && rows->npieces < 32) rows->pieces[rows->npieces++] = gy;
}

static inline void hud_art_rows_slot(HudArtRows *rows, float gy, int slot) {
    if (slot < 0 || slot >= 3 || !isfinite(gy)) return;
    unsigned bit = 1u << slot;
    if ((rows->marker_mask & bit) && fabsf(rows->marker_y[slot] - gy) > 22.0f) rows->bad_markers |= bit;
    rows->marker_mask |= bit;
    rows->marker_y[slot] = gy;
}

static inline float hud_art_row_distance(const HudArtRow *row, float gy) {
    return gy < row->y0 ? row->y0 - gy : gy > row->y1 ? gy - row->y1 : 0.0f;
}

// Finalize one present's observations. Keep this object between adjacent presents so a partial redraw can
// retain a row's display position; the renderer owns the cache's lifetime and clears it after gaps/expiry.
static inline void hud_art_rows_finish(HudArtRows *rows) {
    HudArtRow previous[3];
    int nprevious = rows->nrows;
    for (int i = 0; i < nprevious; i++) previous[i] = rows->rows[i];
    rows->nrows = 0;
    for (int i = 1; i < rows->npieces; i++) {
        float y = rows->pieces[i];
        int j = i;
        while (j && rows->pieces[j - 1] > y) rows->pieces[j] = rows->pieces[j - 1], j--;
        rows->pieces[j] = y;
    }
    bool overflow = false;
    float last = -INFINITY;
    for (int i = 0; i < rows->npieces; i++) {
        float y = rows->pieces[i];
        if (y - last > 22.0f) {
            if (rows->nrows == 3) { overflow = true; break; }
            HudArtRow *row = &rows->rows[rows->nrows++];
            row->y0 = row->y1 = row->anchor = y;
            row->slot = -1;
            row->identified = false;
        }
        rows->rows[rows->nrows - 1].y1 = y;
        last = y;
    }
    if (overflow) rows->nrows = 0;
    unsigned old_used = 0;
    for (int i = 0; i < rows->nrows; i++) {
        HudArtRow *row = &rows->rows[i];
        int match = -1;
        float best = INFINITY;
        for (int j = 0; j < nprevious; j++) {
            if (old_used & (1u << j)) continue;
            float d = hud_art_row_distance(&previous[j], (row->y0 + row->y1) * 0.5f);
            if (d <= 22.0f && d < best) best = d, match = j;
        }
        if (match >= 0) {
            old_used |= 1u << match;
            row->anchor = previous[match].anchor;
            row->slot = previous[match].slot;
            // Retained positions are for display only; only this present's real icons identify an art.
        }
    }
    // Preserve other native rows through partial redraws without refreshing any input identity.
    if (!overflow && rows->nrows)
        for (int i = 0; i < nprevious && rows->nrows < 3; i++)
            if (!(old_used & (1u << i))) {
                rows->rows[rows->nrows] = previous[i];
                rows->rows[rows->nrows++].identified = false;
            }
    unsigned confirmed = 0, conflicting = 0;
    int claimed[3] = {-1, -1, -1};
    for (int slot = 0; slot < 3; slot++) {
        if (!(rows->marker_mask & (1u << slot)) || (rows->bad_markers & (1u << slot))) continue;
        int match = -1;
        float best = INFINITY;
        for (int i = 0; i < rows->nrows; i++) {
            float d = hud_art_row_distance(&rows->rows[i], rows->marker_y[slot]);
            if (d <= 22.0f && d < best) best = d, match = i;
        }
        if (match < 0) continue;
        if (claimed[match] >= 0) conflicting |= 1u << match;
        else claimed[match] = slot;
    }
    for (int i = 0; i < rows->nrows; i++) {
        HudArtRow *row = &rows->rows[i];
        if (conflicting & (1u << i)) row->slot = -1;
        else if (claimed[i] >= 0) {
            row->slot = claimed[i];
            row->anchor = rows->marker_y[row->slot];
            row->identified = true;
            confirmed |= 1u << row->slot;
        }
    }
    unsigned used = confirmed;
    for (int i = 0; i < rows->nrows; i++) {
        HudArtRow *row = &rows->rows[i];
        if (row->identified) continue;
        if (row->slot < 0 || (used & (1u << row->slot))) row->slot = -1;
        else used |= 1u << row->slot;
    }
    for (int i = 0; i < rows->nrows; i++)
        if (rows->rows[i].slot < 0)
            for (int slot = 0; slot < 3; slot++)
                if (!(used & (1u << slot))) { rows->rows[i].slot = slot; used |= 1u << slot; break; }
    rows->npieces = 0;
    rows->marker_mask = rows->bad_markers = 0;
}

static inline bool hud_art_rows_map(const HudArtRows *rows, float gy, float *canvas_y) {
    if (!isfinite(gy) || !canvas_y) return false;
    int match = -1;
    float best = INFINITY;
    for (int i = 0; i < rows->nrows; i++) {
        float d = hud_art_row_distance(&rows->rows[i], gy);
        if (d <= 22.0f && d < best) best = d, match = i;
    }
    if (match < 0) return false;
    const HudArtRow *row = &rows->rows[match];
    // These are the touch HUD's three designed display positions, not inferred native slot coordinates.
    *canvas_y = 41.0f + row->slot * 66.0f + gy - row->anchor;
    return true;
}

// The entire item group includes the bowgun's ammo list above y 500.
static inline int hud_touch_zone(float x, float y, float z, bool hunt_hud) {
    if (!hunt_hud) return HUD_TOUCH_NONE;
    if (fabsf(z - 100.0f) < 0.5f) {
        if (x >= 700.0f && x <= 900.0f && y > 560.0f) return HUD_TOUCH_ARTS;
        if (x >= 1170.0f && x <= 1230.0f && y >= 16.0f && y <= 95.0f) return HUD_TOUCH_TARGET;
    } else if (fabsf(z - 103.0f) < 0.5f && !(y < 500.0f && x < 900.0f)) {
        return HUD_TOUCH_ITEM;
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
