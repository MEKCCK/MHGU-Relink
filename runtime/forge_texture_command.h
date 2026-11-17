// Translate only Forge-owned IDs; restore the snapshot after recording the draw.
#pragma once
#include "imgui.h"
#include "forge_textures.h"

struct ForgeTextureCommand {
    ImDrawCmd *command;
    ImTextureRef texture;
    ImVec4 clip;
    static bool apply(ForgeTextures &textures, ImDrawCmd &command, ForgeTextureCommand &saved) {
        uint64_t id = command.GetTexID();
        if (!ForgeTextures::owns(id)) return false;
        saved = {&command, command.TexRef, command.ClipRect};
        uint64_t descriptor = textures.descriptor(id);
        if (descriptor) command.TexRef = ImTextureRef((ImTextureID)descriptor);
        else command.ClipRect = ImVec4(0, 0, 0, 0);  // the Vulkan backend culls this before binding its descriptor
        return true;
    }
    void restore() const { command->TexRef = texture; command->ClipRect = clip; }
};
