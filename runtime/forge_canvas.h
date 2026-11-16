// Forge free-render geometry uses the game's canvas; menus keep window coordinates.
#pragma once

#include "imgui.h"
#include <algorithm>

namespace forge_canvas {

struct Map {
    float sx, sy, ox, oy;
    float window_w, window_h;
};

inline Map compute(float window_w, float window_h, float game_aspect, float guest_w, float guest_h) {
    IM_ASSERT(window_w > 0 && window_h > 0 && game_aspect > 0 && guest_w > 0 && guest_h > 0);
    float w = window_w, h = w / game_aspect;
    if (h > window_h) h = window_h, w = h * game_aspect;
    return {w / guest_w, h / guest_h, (window_w - w) * .5f, (window_h - h) * .5f, window_w, window_h};
}

struct Checkpoint {
    int vertices = 0, commands = 0, indices = 0;
    unsigned elements = 0;
};

inline Checkpoint checkpoint(const ImDrawList *list) {
    return {list->VtxBuffer.Size, list->CmdBuffer.Size, list->IdxBuffer.Size,
            list->CmdBuffer.empty() ? 0 : list->CmdBuffer.back().ElemCount};
}

// Call after the plugin's direct guest vertex edits have been synchronized. A fresh/reset list uses {}.
// ImGui can append the plugin's triangles to the UI's last command: split that command at its old index count.
inline void transform(ImDrawList *list, Checkpoint before, const Map &map) {
    IM_ASSERT(before.vertices <= list->VtxBuffer.Size && before.indices <= list->IdxBuffer.Size);
    for (int i = before.vertices; i < list->VtxBuffer.Size; ++i) {
        ImVec2 &p = list->VtxBuffer[i].pos;
        p = ImVec2(p.x * map.sx + map.ox, p.y * map.sy + map.oy);
    }
    for (int i = 0; i < list->CmdBuffer.Size; ++i) {
        ImDrawCmd &cmd = list->CmdBuffer[i];
        bool geometry = cmd.ElemCount && cmd.IdxOffset + cmd.ElemCount > (unsigned)before.indices;
        bool callback = cmd.UserCallback && i >= before.commands - 1 && cmd.IdxOffset >= (unsigned)before.indices;
        if (!geometry && !callback) continue;  // keep old UI and the restored physical empty tail untouched
        if (geometry && cmd.IdxOffset < (unsigned)before.indices) {
            ImDrawCmd added = cmd;
            unsigned prefix = before.indices - cmd.IdxOffset;
            added.IdxOffset += prefix;
            added.ElemCount -= prefix;
            cmd.ElemCount = prefix;
            list->CmdBuffer.insert(list->CmdBuffer.Data + i + 1, added);
            ++i;
        }
        ImVec4 &r = list->CmdBuffer[i].ClipRect;
        r = ImVec4(std::clamp(r.x * map.sx + map.ox, 0.f, map.window_w),
                   std::clamp(r.y * map.sy + map.oy, 0.f, map.window_h),
                   std::clamp(r.z * map.sx + map.ox, 0.f, map.window_w),
                   std::clamp(r.w * map.sy + map.oy, 0.f, map.window_h));
    }
}

}  // namespace forge_canvas
