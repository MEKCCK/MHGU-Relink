// Controller navigation exercised against the real ImGui frame loop.
#include "settings_panel.h"
#include "config.h"
#include "app_actions.h"
#include "overlay.h"
#include "modorder.h"
#include "padmap.h"
#include "textinput.h"
#include "touch.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
static int requested;
extern "C" const char *g_game_dir = "obj/settings-test/game";  // (modorder.c compares archives with the game's)
extern "C" volatile uint64_t g_present_count = 0;  // (runtime/ui.c's frame counter: config.c's model previews read it)
extern "C" void ui_request_action(bool restart) { requested = restart ? 2 : 1; }
extern "C" bool ui_action_failed(void) { return false; }
extern "C" int host_input_last_device(void) { return 0; }
extern "C" int host_input_sw_sources(int, int, int *, int) { return 0; }
extern "C" int host_input_held(int *, int) { return 0; }
extern "C" bool input_view_dash_zr(void) { return false; }
extern "C" void input_view_apply(void) {}  // (the game side of the action layer: nothing to hook here)
static int import_requested;
static const char *picked_folder;  // (what the folder picker "chose": ui_take_picked_folder hands it out once)
extern "C" void ui_request_import(int what) { import_requested = what; }
extern "C" int ui_take_picked_folder(char *out, size_t size) {
    if (!picked_folder) return 0;
    SDL_strlcpy(out, picked_folder, size);
    picked_folder = nullptr;
    return 4;
}
extern "C" int ldn_hub_status(const char *, const char *, int *, int *) { return 0; }
extern "C" void ldn_hub_check(const char *, const char *) {}
extern "C" bool forge_plugins_have(int) { return false; }  // (no Forge plugins started)
extern "C" int forge_plugin_titles(const char **, int) { return 0; }
// (cheats.c: two cheats, a master code and one that can be turned off)
static bool cheat_on[2] = {true, true};
extern "C" int cheats_count(void) { return 2; }
extern "C" void cheats_rescan(void) {}
extern "C" const char *cheats_name(int i) { return i ? "Test cheat" : "Master"; }
extern "C" const char *cheats_file(int i) { (void)i; return "test.txt"; }
extern "C" bool cheats_master(int i) { return i == 0; }
extern "C" bool cheats_on(int i) { return cheat_on[i]; }
extern "C" bool cheats_set(const bool *on, int n, char *, size_t) {
    for (int i = 0; i < n && i < 2; i++)
        cheat_on[i] = on[i];
    return true;
}
extern "C" bool cheats_add(const char *, char *err, size_t n) {  // (Add a cheat: not reached here)
    snprintf(err, n, "not in the test");
    return false;
}
extern "C" int overlay_face_position(int semantic) {
    const int p[] = {1, 0, 3, 2};
    return p[semantic];
}
extern "C" bool padmap_active(void) { return false; }
extern "C" void padmap_watch(void) {}
extern "C" int padmap_list(char[][64], bool *, int) { return 0; }
extern "C" void padmap_begin(int) {}
extern "C" int padmap_step(void) { return 0; }
extern "C" int padmap_steps(void) { return 0; }
extern "C" const char *padmap_step_title(int) { return ""; }
extern "C" void padmap_continue(void) {}
extern "C" void padmap_skip(void) {}
extern "C" void padmap_back(void) {}
extern "C" void padmap_save(void) {}
extern "C" void padmap_cancel(void) {}
extern "C" void padmap_forget(int) {}
extern "C" void textinput_menu_keyboard(bool) {}
extern "C" void textinput_menu_reopen(void) {}
static const char *const preset_names[TOUCH_PRESET_COUNT] = {
    "Great Sword", "Sword & Shield", "Hammer", "Lance", "Heavy Bowgun", "Light Bowgun", "Long Sword",
    "Switch Axe", "Gunlance", "Bow", "Dual Blades", "Hunting Horn", "Insect Glaive", "Charge Blade", "Prowler",
    "Universal (virtual buttons)", "Reserved preset", "Reserved preset", "Reserved preset", "Reserved preset"
};
static int preset_bindings[TOUCH_PRESET_COUNT] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,-1,-1,-1,-1,-1};
static int current_preset = -1;
static int preset_name_queries, preset_binding_queries, preset_current_queries, edit_target = -1, edit_cancelled;
static bool preset_use_ok = true;
struct PresetUse { int preset, weapon; };
static std::vector<PresetUse> preset_uses;
extern "C" const char *touch_preset_name(int preset) {
    ++preset_name_queries;
    assert(preset >= 0 && preset < TOUCH_PRESET_COUNT);
    return preset_names[preset];
}
extern "C" int touch_preset_binding(int preset) {
    ++preset_binding_queries;
    assert(preset >= 0 && preset < TOUCH_PRESET_COUNT);
    return preset_bindings[preset];
}
extern "C" int touch_preset_current(void) {
    ++preset_current_queries;
    return current_preset;
}
extern "C" bool touch_preset_use(int preset, int weapon) {
    assert(preset >= 0 && preset < TOUCH_PRESET_COUNT);
    preset_uses.push_back({preset, weapon});
    if (!preset_use_ok) return false;
    if (preset > TOUCH_UNIVERSAL_PRESET && weapon >= 0) preset_bindings[preset] = weapon;
    if (preset > TOUCH_UNIVERSAL_PRESET) {
        int bound = preset_bindings[preset];
        if (bound < 0 || bound >= TOUCH_UNIVERSAL_PRESET) return false;
    }
    current_preset = preset;
    return true;
}
extern "C" void touch_edit_begin(void) { edit_target = -2; }
extern "C" void touch_edit_preset(int preset) { edit_target = preset; }
extern "C" void touch_edit_cancel(void) { edit_target = -1; ++edit_cancelled; }
static bool opened = true;
static void frame() {
    settings_panel_before_frame(opened);
    ImGui::NewFrame();
    settings_panel(&opened);
    ImGui::Render();
}
static void idle(int n = 3) {
    while (n--)
        frame();
}
static void press(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    idle();
    ImGui::GetIO().AddKeyEvent(key, false);
    idle();
}
static ImGuiID id(const char *name) {  // (a dropdown's choices are in a window of their own)
    auto *w = ImGui::FindWindowByName(strncmp(name, "choice.", 7) ? "Settings##main" : "Settings##dropdown");
    assert(w);
    return ImHashStr("###row", 0, ImHashStr(name, 0, w->ID));
}
static void selected(const char *name) {
    auto &g = *ImGui::GetCurrentContext();
    if (g.NavId != id(name))
        fprintf(stderr, "focus %s: %x != %x\n", name, g.NavId, id(name));
    assert(g.NavId == id(name));
    assert(g.NavCursorVisible);
}
static float pinned_content_bottom() {
    auto *w = ImGui::FindWindowByName("Settings##main");
    float bottom = 0;
    for (const auto &command : w->DrawList->CmdBuffer) {
        auto clip = command.ClipRect;
        if (command.ElemCount && clip.x == w->InnerClipRect.Min.x && clip.z == w->InnerClipRect.Max.x &&
            clip.w < w->InnerClipRect.Max.y - 1 && clip.w > bottom)
            bottom = clip.w;
    }
    assert(bottom > w->InnerClipRect.Min.y);
    return bottom;
}
static float pinned_content_top() {
    auto *w = ImGui::FindWindowByName("Settings##main");
    float top = 0;
    for (const auto &command : w->DrawList->CmdBuffer) {
        auto clip = command.ClipRect;
        if (command.ElemCount && clip.x == w->InnerClipRect.Min.x && clip.z == w->InnerClipRect.Max.x &&
            clip.w < w->InnerClipRect.Max.y - 1 && clip.y > top)
            top = clip.y;
    }
    assert(top > w->InnerClipRect.Min.y);
    return top;
}
static void down(int n = 1) {
    while (n--)
        press(ImGuiKey_GamepadDpadDown);
}
static void tab() { press(ImGuiKey_GamepadR1); }
static void back() { press(ImGuiKey_GamepadFaceRight); }
static void confirm() { press(ImGuiKey_GamepadFaceDown); }
// Focus through the real rows rather than depending on their current positions in Controls.
static bool focused_row(const char *name) {
    auto &g = *ImGui::GetCurrentContext();
    return g.NavWindow && g.NavId == ImHashStr("###row", 0, ImHashStr(name, 0, g.NavWindow->ID));
}
static void seek_row(const char *name) {
    press(ImGuiKey_Home);
    for (int i = 0; i < 100; i++) {
        if (focused_row(name)) return;
        down();
    }
    fprintf(stderr, "Could not reach settings row: %s\n", name);
    assert(false);
}
static void click_focused_row() {
    idle(8); // Allow the menu's scroll-to-focus request to finish before the real click.
    auto &g = *ImGui::GetCurrentContext();
    assert(g.NavWindow);
    ImRect rect = g.NavWindow->NavRectRel[ImGuiNavLayer_Main];
    ImVec2 center = rect.GetCenter();
    center.x += g.NavWindow->Pos.x;
    center.y += g.NavWindow->Pos.y;
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(center.x, center.y);
    idle();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    idle();
}
static void open_presets() {
    seek_row("touch.presets");
    confirm();
    assert(opened && !settings_panel_confirming());
}
static void expect_use(int preset, int weapon, size_t previous) {
    assert(preset_uses.size() == previous + 1);
    assert(preset_uses.back().preset == preset && preset_uses.back().weapon == weapon);
    assert(opened && !settings_panel_confirming());
}
static void preset_regression() {
    idle();
    tab();
    tab();
    assert(!settings_panel_dirty());
    current_preset = 9; // A saved selection must win over the menu's initial/previous edit target.
    open_presets();
    assert(preset_name_queries == TOUCH_PRESET_COUNT && preset_binding_queries == TOUCH_PRESET_COUNT);
    assert(preset_current_queries == 1 && focused_row("tp.9"));
    int queries = preset_name_queries;
    int current_queries = preset_current_queries;
    idle(20);
    assert(preset_name_queries == queries && preset_binding_queries == queries && preset_current_queries == current_queries);
    for (int i = 0; i < TOUCH_PRESET_COUNT; i++) {
        std::string row = "tp." + std::to_string(i);
        seek_row(row.c_str());
    }
    assert(preset_uses.empty()); // Moving focus never applies a preset.
    seek_row("tp.0");
    click_focused_row();
    expect_use(0, -1, 0);
    assert(current_preset == 0);
    seek_row("tp.14");
    confirm();
    expect_use(14, -1, 1);
    assert(current_preset == 14);
    seek_row("tp.15");
    click_focused_row();
    expect_use(TOUCH_UNIVERSAL_PRESET, -1, 2);
    assert(current_preset == TOUCH_UNIVERSAL_PRESET);
    assert(!settings_panel_dirty()); // Direct use needs no settings Save.
    queries = preset_name_queries;
    current_queries = preset_current_queries;
    idle(20);
    assert(preset_name_queries == queries && preset_binding_queries == queries && preset_current_queries == current_queries);
    size_t used = preset_uses.size();
    seek_row("tp.16");
    confirm();
    assert(preset_uses.size() == used && focused_row("tp.bind"));
    assert(current_preset == TOUCH_UNIVERSAL_PRESET && preset_current_queries == current_queries);
    confirm();
    assert(focused_row("choice.0"));
    confirm(); // Choosing the already highlighted first base must still initialize and apply.
    expect_use(16, 0, used);
    assert(preset_bindings[16] == 0 && current_preset == 16);
    used = preset_uses.size();
    seek_row("tp.16");
    click_focused_row();
    expect_use(16, -1, used);
    assert(preset_bindings[16] == 0 && current_preset == 16); // A saved slot is used without copying its base again.
    for (int preset = 17; preset < TOUCH_PRESET_COUNT; preset++) {
        used = preset_uses.size();
        std::string row = "tp." + std::to_string(preset);
        seek_row(row.c_str());
        confirm();
        assert(preset_uses.size() == used && focused_row("tp.bind"));
        confirm();
        seek_row("choice.14");
        confirm();
        expect_use(preset, 14, used);
        assert(preset_bindings[preset] == 14 && current_preset == preset);
    }
    used = preset_uses.size();
    seek_row("tp.bind");
    confirm();
    seek_row("choice.6");
    confirm();
    expect_use(19, 6, used);
    assert(preset_bindings[19] == 6 && current_preset == 19);
    seek_row("tp.0");
    preset_use_ok = false;
    used = preset_uses.size();
    queries = preset_name_queries;
    current_queries = preset_current_queries;
    confirm();
    expect_use(0, -1, used);
    assert(current_preset == 19 && preset_name_queries == queries && preset_binding_queries == queries &&
           preset_current_queries == current_queries);
    settings_panel_before_frame(opened);
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    settings_panel(&opened);
    assert(strstr(ImGui::GetCurrentContext()->LogBuffer.c_str(), "Could not apply this preset."));
    ImGui::LogFinish();
    ImGui::Render();
    preset_use_ok = true;
    seek_row("tp.edit");
    confirm();
    assert(!opened && edit_target == 0 && current_preset == 19); // Edit target and current selection are separate.
    frame();
    opened = true;
    idle();
    open_presets();
    assert(focused_row("tp.19") && current_preset == 19); // Re-entry follows runtime, including after a failed use.
    seek_row("tp.15");
    confirm();
    seek_row("tp.edit");
    click_focused_row();
    assert(!opened && edit_target == 15); // Editing closes the whole overlay from this subpage.
    frame();
    opened = true;
    idle();
    seek_row("touch.hud");
    confirm();
    assert(settings_panel_dirty());
    open_presets();
    seek_row("tp.edit");
    confirm();
    assert(opened && settings_panel_confirming() && edit_target == 15);
    int cancelled = edit_cancelled;
    confirm(); // Cancel is the dialog's default.
    assert(opened && !settings_panel_confirming() && edit_target == -1 && edit_cancelled == cancelled + 1 && current_preset == 15);
    assert(focused_row("tp.edit"));
    confirm();
    assert(settings_panel_confirming());
    back();
    assert(opened && !settings_panel_confirming() && edit_target == -1 && edit_cancelled == cancelled + 2 && current_preset == 15);
    confirm();
    seek_row("dialog.discard");
    confirm();
    assert(!opened && edit_target == 15 && current_preset == 15);
    frame();
    opened = true;
    idle();
    seek_row("touch.hud");
    confirm();
    assert(settings_panel_dirty());
    open_presets();
    seek_row("tp.5");
    confirm();
    seek_row("tp.edit");
    confirm();
    assert(settings_panel_confirming() && edit_target == 5);
    seek_row("dialog.save");
    confirm();
    assert(!opened && edit_target == 5 && current_preset == 5);
    frame();
    assert(!settings_panel_dirty());
    puts("PASS: real settings-panel ImGui loop: 20 directly usable presets; saved/current selection and edit target stay separate across failure and re-entry; reserved initialization and explicit base changes; cached current/name/base queries; exact editor and Save/Discard/Cancel/Back (host stubs, no Android/game execution)");
}
static void combo_layout_regression() {
    static_assert(TOUCH_WEAPON_LAYOUTS == 15 && TOUCH_LEGACY_LAYOUTS == 4 && TOUCH_LAYOUT_PROFILES == 19,
                  "Removing choices must not renumber the saved combo profiles");
    Config fixture = config_editable();
    memset(fixture.touch_weapon_combo, 0, sizeof fixture.touch_weapon_combo);
    memset(fixture.touch_weapon_combo_pad, 0, sizeof fixture.touch_weapon_combo_pad);
    strcpy(fixture.touch_weapon_combo[15][0], "L+ZL");
    strcpy(fixture.touch_weapon_combo[17][0], "R+ZR");
    fixture.touch_weapon_combo_pad[15][0] = PADB_L3;
    fixture.touch_weapon_combo_pad[17][0] = PADB_R3;
    config_store_loaded(&fixture);
    config_runtime_init(&fixture);
    config_set_touch_profile(15); // A retired saved selection falls back to Melee layout 2.
    opened = true;
    idle();
    seek_row("touch.buttons");
    confirm();
    seek_row("tc.0");
    confirm();
    seek_row("ce.weapon");
    confirm();
    assert(focused_row("choice.15"));
    press(ImGuiKey_End);
    assert(focused_row("choice.16")); // Exactly 15 weapon choices plus the two surviving general layouts.
    seek_row("choice.15");
    confirm();
    seek_row("ce.0");
    confirm();
    seek_row("ce.pad");
    confirm();
    seek_row("choice.1");
    confirm();
    seek_row("ce.weapon");
    confirm();
    seek_row("choice.16");
    confirm();
    seek_row("ce.1");
    confirm();
    seek_row("ce.pad");
    confirm();
    seek_row("choice.2");
    confirm();
    for (int weapon = 0; weapon < TOUCH_WEAPON_LAYOUTS; weapon++) {
        seek_row("ce.weapon");
        confirm();
        std::string row = "choice." + std::to_string(weapon);
        seek_row(row.c_str());
        confirm();
        seek_row("ce.0");
        confirm();
    }
    back();
    back();
    seek_row("save");
    confirm();
    assert(!settings_panel_dirty());
    Config saved = config_editable();
    for (int weapon = 0; weapon < TOUCH_WEAPON_LAYOUTS; weapon++)
        assert(config_combo_mask(saved.touch_weapon_combo[weapon][0]) == config_combo_mask("A"));
    assert(config_combo_mask(saved.touch_weapon_combo[16][0]) == config_combo_mask("A"));
    assert(saved.touch_weapon_combo_pad[16][0] == PADB_L3);
    assert(config_combo_mask(saved.touch_weapon_combo[18][0]) == config_combo_mask("B"));
    assert(saved.touch_weapon_combo_pad[18][0] == PADB_R3);
    for (int retired : {15, 17}) {
        assert(!memcmp(saved.touch_weapon_combo[retired], fixture.touch_weapon_combo[retired],
                       sizeof saved.touch_weapon_combo[retired]));
        assert(!memcmp(saved.touch_weapon_combo_pad[retired], fixture.touch_weapon_combo_pad[retired],
                       sizeof saved.touch_weapon_combo_pad[retired]));
    }
    opened = false;
    frame();
    config_set_touch_profile(17);
    opened = true;
    idle();
    seek_row("touch.buttons");
    confirm();
    seek_row("tc.0");
    confirm();
    seek_row("ce.weapon");
    confirm();
    assert(focused_row("choice.15"));
    back();
    assert(!settings_panel_dirty()); // Merely opening a retired profile never overwrites its old data.
    opened = false;
    frame();
    puts("PASS: real settings-panel combo selector: 15 unchanged weapon slots plus UI 15->profile 16 and UI 16->profile 18; retired profiles 15/17 preserved and safely defaulted (host stubs)");
}
// Capture the actual phone-style settings draw lists on the host, not a mockup or an Android/game screenshot.
static void preset_previews() {
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
    SDL_Surface *surface = SDL_CreateSurface(640, 360, SDL_PIXELFORMAT_RGBA32);
    assert(surface);
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
    assert(renderer);
    auto &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640, 360);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    auto font = std::filesystem::path(__FILE__).parent_path().parent_path() / "android/app/src/main/assets/cjk.ttf";
    io.FontDefault = io.Fonts->AddFontFromFileTTF(font.string().c_str(), 20);
    assert(io.FontDefault);
    Config translated = config_editable();
    translated.language = 2;
    config_store_loaded(&translated);
    config_runtime_init(&translated);
    opened = true;
    idle();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(1, 1);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    idle();
    open_presets();
    std::vector<ImTextureData *> textures;
    auto capture = [&](const char *path) {
        idle(8);
        ImDrawData *data = ImGui::GetDrawData();
        assert(data && data->TotalVtxCount > 100);
        if (data->Textures) for (ImTextureData *tex : *data->Textures) {
            if (tex->Status == ImTextureStatus_WantCreate) {
                assert(tex->Format == ImTextureFormat_RGBA32);
                SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                    SDL_TEXTUREACCESS_STATIC, tex->Width, tex->Height);
                assert(texture);
                assert(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND));
                assert(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR));
                assert(SDL_UpdateTexture(texture, nullptr, tex->GetPixels(), tex->GetPitch()));
                tex->SetTexID((ImTextureID)(uintptr_t)texture);
                tex->SetStatus(ImTextureStatus_OK);
                textures.push_back(tex);
            } else if (tex->Status == ImTextureStatus_WantUpdates) {
                assert(SDL_UpdateTexture((SDL_Texture *)(uintptr_t)tex->TexID, nullptr, tex->GetPixels(), tex->GetPitch()));
                tex->SetStatus(ImTextureStatus_OK);
            }
        }
        assert(SDL_SetRenderClipRect(renderer, nullptr));
        assert(SDL_SetRenderDrawColor(renderer, 29, 43, 56, 255));
        assert(SDL_RenderClear(renderer));
        for (const ImDrawList *list : data->CmdLists) {
            std::vector<SDL_Vertex> vertices;
            vertices.reserve(list->VtxBuffer.Size);
            for (const ImDrawVert &v : list->VtxBuffer) vertices.push_back({{v.pos.x, v.pos.y},
                {((v.col >> IM_COL32_R_SHIFT) & 255) / 255.f, ((v.col >> IM_COL32_G_SHIFT) & 255) / 255.f,
                 ((v.col >> IM_COL32_B_SHIFT) & 255) / 255.f, ((v.col >> IM_COL32_A_SHIFT) & 255) / 255.f}, {v.uv.x, v.uv.y}});
            for (const ImDrawCmd &cmd : list->CmdBuffer) {
                assert(!cmd.UserCallback);
                SDL_Rect clip{(int)cmd.ClipRect.x, (int)cmd.ClipRect.y,
                    (int)(cmd.ClipRect.z - cmd.ClipRect.x), (int)(cmd.ClipRect.w - cmd.ClipRect.y)};
                assert(SDL_SetRenderClipRect(renderer, &clip));
                std::vector<int> indices(cmd.ElemCount);
                for (unsigned i = 0; i < cmd.ElemCount; ++i)
                    indices[i] = list->IdxBuffer[cmd.IdxOffset + i] + cmd.VtxOffset;
                assert(SDL_RenderGeometry(renderer, (SDL_Texture *)(uintptr_t)cmd.GetTexID(), vertices.data(),
                    (int)vertices.size(), indices.data(), (int)indices.size()));
            }
        }
        assert(SDL_FlushRenderer(renderer));
        assert(SDL_SavePNG(surface, path));
        printf("HOST PREVIEW: %s (640x360, Chinese phone-style settings, actual ImGui draw lists / SDL software renderer; no Android/game execution)\n", path);
    };
    seek_row("tp.15");
    confirm();
    capture("settings-presets-universal-zh-640x360.png");
    seek_row("tp.edit");
    capture("settings-presets-edit-zh-640x360.png");
    for (ImTextureData *tex : textures) {
        SDL_DestroyTexture((SDL_Texture *)(uintptr_t)tex->TexID);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
    opened = false;
    frame();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    SDL_Quit();
}
int main(int argc, char **argv) {
    const bool touch_presets_only = argc > 1 && !strcmp(argv[1], "--touch-presets");
    if (touch_presets_only) setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "obj/settings-test/panel-settings.ini");
#else
    setenv("MHGU_CONFIG", "obj/settings-test/panel-settings.ini", 1);
#endif
    // Mods tab fixtures: Aaa (two files) and Bbb (one) both replace a.bin, Ccc stands alone, Ddd is off ('_')
    namespace fs = std::filesystem;
    fs::remove_all("obj/settings-test/mods");
    auto put = [](const char *path, const char *text) {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream(path, std::ios::binary) << text;
    };
    put("obj/settings-test/mods/Aaa/romfs/a.bin", "1");
    put("obj/settings-test/mods/Aaa/romfs/b.bin", "bb");
    put("obj/settings-test/mods/Bbb/romfs/a.bin", "22");
    put("obj/settings-test/mods/Ccc/romfs/c.bin", "3");
    put("obj/settings-test/mods/_Ddd/romfs/d.bin", "4");
#ifdef _WIN32
    _putenv_s("MHGU_MODS_DIR", "obj/settings-test/mods");
#else
    setenv("MHGU_MODS_DIR", "obj/settings-test/mods", 1);
#endif
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (touch_presets_only) {
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefaultVector();
    } else {
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
    }
    Config baseline = config_defaults();
    config_store_loaded(&baseline);
    config_runtime_init(&baseline);

    if (touch_presets_only) {
        preset_regression();
        combo_layout_regression();
        preset_previews();
        ImGui::DestroyContext();
        remove("obj/settings-test/panel-settings.ini");
        return 0;
    }

    idle();
    selected("display.mode");
    // A click outside the menu unfocuses every window; the menu takes its focus back (it used to assert in SetNavID)
    io.AddMousePosEvent(4, 4);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    idle();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    idle();
    selected("display.mode");
    confirm();
    selected("choice.2");
    back();
    selected("display.mode");
    down(2);
    selected("display.width");
    confirm();
    selected("display.width");
    assert(opened && !settings_panel_confirming() &&
           !settings_panel_dirty()); // Numeric activation never opens an editor.
    press(ImGuiKey_GamepadDpadRight);
    assert(settings_panel_dirty());
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    frame();
    opened = true;
    idle();
    selected("display.mode");
    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    idle(35);
    selected("volume");  // (Audio first)
    down(3);  // (mutebg, show.fps, fps)
    selected("fps");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    idle();
    // The frame rate: Confirm lists the common ones, left / right go a frame at a time
    confirm();
    selected("choice.1");  // (60, the one it has)
    down();
    confirm();
    selected("fps");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_editable().fps == 91);
    press(ImGuiKey_Home);
    down(3);
    selected("fps");
    confirm();
    selected("choice.2");  // (91 is none of them: the one below)
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_editable().fps == 60);
    press(ImGuiKey_Home);
    down(3);
    confirm();
    selected("choice.1");
    back();  // (Back closes the dropdown, nothing changed)
    selected("fps");
    assert(!settings_panel_dirty() && opened);
    tab();
    selected("face");
    // The Keybinds submenu: a key chosen for B, then a mouse button as its second; the recomp's own keys refused
    press(ImGuiKey_Home);
    down(9);  // (background, keyboard prompts, combo, mouse, vertical, invert, smoothing, capture, keybinds)
    selected("keybinds");
    confirm();
    selected("bind.0");  // (past the section's title)
    down(6);             // (past the Buttons title)
    selected("bind.6");
    confirm();
    selected("choice.0");  // Set key
    confirm();
    selected("capture.cancel");
    assert(settings_panel_key(SDL_SCANCODE_F12));
    idle();
    selected("capture.cancel");  // (refused: still choosing)
    assert(settings_panel_key(SDL_SCANCODE_G));
    idle();
    selected("bind.6");
    confirm();
    down();
    selected("choice.1");  // Add a second key
    confirm();
    assert(settings_panel_mouse(SDL_BUTTON_MIDDLE));
    idle();
    selected("bind.6");
    assert(!settings_panel_mouse(SDL_BUTTON_LEFT));  // (not choosing: a click is the menu's)
    // The pointer on the capture view's Cancel: a left click is that row's, a side button (Mouse 4) still the key
    down();
    selected("bind.7");
    confirm();
    selected("choice.0");  // Set key
    confirm();
    selected("capture.cancel");
    {
        ImGuiWindow *w = ImGui::GetCurrentContext()->NavWindow;
        ImRect r = ImGui::WindowRectRelToAbs(w, w->NavRectRel[0]);
        ImGui::GetIO().AddMousePosEvent(r.GetCenter().x, r.GetCenter().y);
        idle();
    }
    assert(!settings_panel_mouse(SDL_BUTTON_LEFT));
    assert(settings_panel_mouse(SDL_BUTTON_X1));
    idle();
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    idle();
    selected("bind.7");
    back();
    selected("keybinds");
    assert(settings_panel_dirty());
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_live_bind(BIND_B, 0) == SDL_SCANCODE_G && config_live_bind(BIND_B, 1) == KEY_MOUSE + SDL_BUTTON_MIDDLE);
    assert(config_live_bind(BIND_X, 0) == KEY_MOUSE + SDL_BUTTON_X1);
    press(ImGuiKey_Home);
    down(9);
    confirm();
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("bind.reset");
    confirm();
    back();
    selected("keybinds");
    // The Game actions screen: the tabs over the drawing (Hunt | Town and Hub | Menus | Keys) answer the mouse
    down();
    selected("actions");
    confirm();
    selected("act.enable");
    {
        auto *bw = ImGui::FindWindowByName("Settings##bind");
        assert(bw);
        float y = bw->Pos.y + ImGui::GetStyle().WindowPadding.y + bw->FontWindowScale * ImGui::GetFontSize();
        bool found = false;
        for (float x = bw->Pos.x + 60; x < bw->Pos.x + bw->Size.x - 60 && !found; x += 8) {
            io.AddMousePosEvent(x, y);
            idle();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            idle(4);
            found = ImGui::GetCurrentContext()->NavId == id("act.tab");  // (a tab click puts the focus on the list's Show row)
        }
        assert(found);  // (a tab of the drawing took a click)
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        idle();
    }
    back();
    selected("actions");
    press(ImGuiKey_GamepadDpadUp);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_live_bind(BIND_B, 0) == SDL_SCANCODE_SPACE && !config_live_bind(BIND_B, 1));
    tab();
    selected("port");
    press(ImGuiKey_GamepadL1);
    selected("face");
    tab();
    tab();
    selected("sync");  // (the Hub heading first)
    down(4);  // (lag compensation, village quests, urgent quests, then the HP heading is passed)
    selected("hp.0");
    tab();
    selected("mod.Aaa");
    // The Mods tab: the conflict scan runs on another thread; Aaa and Bbb both replace a.bin
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    idle();
    down(4);  // (Aaa, Bbb, Ccc, then _Ddd that is off)
    selected("mods.conflicts");  // one row for them all
    down();
    selected("mods.rescan");  // (the suggested order is the one it has: Aaa changes more files, so Bbb comes after)
    press(ImGuiKey_Home);
    down();
    selected("mod.Bbb");
    confirm();
    down();  // Move up
    confirm();
    selected("mod.Bbb");
    assert(settings_panel_dirty());
    press(ImGuiKey_Home);
    selected("mod.Bbb");
    down(5);  // (Bbb, Aaa, Ccc, _Ddd, the conflict)
    selected("mods.suggest");  // Bbb now wins over the bigger Aaa: the order is suggested back
    confirm();
    press(ImGuiKey_Home);
    selected("mod.Aaa");
    down(2);
    selected("mod.Ccc");
    press(ImGuiKey_GamepadDpadLeft);  // off
    assert(settings_panel_dirty());
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(!settings_panel_dirty());
    {
        std::ifstream in("obj/settings-test/mods/load_order.txt");
        std::string list((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(list.find("\nAaa\nBbb\n") != std::string::npos);
        assert(list.find("\nCcc\n") == std::string::npos && list.find("# off: Ccc\n") != std::string::npos);
        assert(list.find("\n_Ddd\n") == std::string::npos);
    }
    // Choosing which of two conflicting mods is used moves it below the other
    press(ImGuiKey_Home);
    down(4);  // (Aaa, Bbb, then the off ones _Ddd and Ccc)
    selected("mods.conflicts");
    confirm();
    selected("mods.conflict.0");
    confirm();
    selected("choice.1");  // (Use Bbb, the one used now)
    press(ImGuiKey_GamepadDpadUp);
    selected("choice.0");  // Use Aaa
    confirm();
    selected("mods.conflict.0");
    down();
    selected("conflicts.back");
    confirm();
    selected("mods.conflicts");
    assert(settings_panel_dirty());
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    {
        std::ifstream in("obj/settings-test/mods/load_order.txt");
        std::string list((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(list.find("\nBbb\nAaa\n") != std::string::npos);
    }
    // A mod that is off keeps its place in the list: turned on again, it loads where it was
    {
        ModEntry order[] = {{"Bbb", true}, {"Ccc", false}, {"Aaa", true}, {"_Ddd", false}};
        char error[256];
        assert(mods_save_order(order, 4, error, sizeof error));
        ModEntry list[MODS_MAX];
        assert(mods_installed(list, MODS_MAX) == 4);
        for (int i = 0; i < 4; i++)
            assert(!strcmp(list[i].name, order[i].name) && list[i].on == order[i].on);
    }
    // A mod folder added after the Mods tab saved loads (on), after the ones it lists
    put("obj/settings-test/mods/Eee/romfs/e.bin", "5");
    {
        ModEntry list[MODS_MAX];
        int n = mods_installed(list, MODS_MAX);
        assert(n == 5);
        const char *want[] = {"Bbb", "Ccc", "Aaa", "_Ddd", "Eee"};
        const bool on[] = {true, false, true, false, true};
        for (int i = 0; i < n; i++)
            assert(!strcmp(list[i].name, want[i]) && list[i].on == on[i]);
    }
    fs::remove_all("obj/settings-test/mods/Eee");
    tab();
    selected("cheats.add");  // (the Cheats tab: Add a cheat, then the cheats)
    tab();
    selected("language");
    tab();
    selected("display.mode");
    tab();
    selected("volume");
    confirm();
    selected("volume");
    press(ImGuiKey_GamepadDpadLeft);
    assert(settings_panel_dirty());
    settings_panel_request_back();
    idle();
    selected("dialog.cancel");
    back();
    selected("volume");
    settings_panel_request_close();
    idle();
    selected("dialog.cancel");
    assert(!requested);
    back();
    selected("volume");
    // Former save/discard shortcuts do nothing, including while held.
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    idle(50);
    assert(!ImGui::GetCurrentContext()->NavWindowingTarget);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
    idle();
    assert(settings_panel_dirty());
    press(ImGuiKey_GamepadFaceUp);  // (Y, the top face button, saves)
    assert(!settings_panel_dirty());
    assert(config_editable().volume == 99);
    selected("reset");  // (as after Save)
    press(ImGuiKey_Home);
    selected("volume");
    press(ImGuiKey_GamepadDpadRight);  // (100 again, not saved)
    assert(settings_panel_dirty());
    press(ImGuiKey_End);
    selected("reset");
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(!settings_panel_dirty());
    assert(config_editable().volume == 100);
    press(ImGuiKey_Home);
    selected("volume");
    press(ImGuiKey_GamepadDpadLeft);
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    assert(config_editable().volume == 100);
    frame();
    opened = true;
    idle();
    selected("volume");
    tab();
    tab();
    selected("port");
    confirm();
    selected("port");
    press(ImGuiKey_GamepadDpadRight);
    assert(settings_panel_dirty());
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "missing-settings-parent/panel.ini");
#else
    setenv("MHGU_CONFIG", "missing-settings-parent/panel.ini", 1);
#endif
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(settings_panel_dirty());
    assert(config_editable().ldn_port == 39500);
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "obj/settings-test/panel-settings.ini");
#else
    setenv("MHGU_CONFIG", "obj/settings-test/panel-settings.ini", 1);
#endif
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    frame();
    opened = true;
    idle();
    selected("port");
    down();
    selected("hosts");
    confirm();
    selected("key.0");
    io.AddInputCharactersUTF8("localhost:39500");
    idle();
    settings_panel_request_close();
    idle();
    selected("dialog.cancel");
    assert(settings_panel_dirty());
    back();
    selected("key.0");
    back();
    selected("hosts");
    assert(!settings_panel_dirty());
    settings_panel_request_close();
    idle();
    selected("dialog.cancel");
    down();
    confirm();
    assert(requested == 1);
    back();
    selected("hosts");
    requested = 0;
    tab();
    tab();
    tab();
    tab();  // (Misc, Mods, Cheats, Menu Options)
    selected("language");
    // The menu's confirm button (Menu controls): saved, it applies to the menu right away
    down(4);  // (theme, scale, detach, menu.confirm)
    selected("menu.confirm");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_SOUTH);
    assert(config_editable().face_buttons == FACE_POSITION);
    assert(!config_restart_required(&baseline));
    press(ImGuiKey_Home);
    down(4);
    selected("menu.confirm");
    press(ImGuiKey_GamepadDpadLeft);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_ORIGINAL);
    press(ImGuiKey_Home);
    selected("language");
    // Save folder (PC): Choose folder opens the system's picker, whose folder lands in the draft; Save keeps it
    down(8);  // (theme, scale, detach, menu.confirm, icons, confirm.exit, updates, bonus)
    selected("bonus");
    down();
    selected("save.location");
    down();
    selected("save.location.pick");
    confirm();
    assert(import_requested == 4);
    picked_folder = "/games/MHGU saves";
    idle();
    picked_folder = "/bad;folder";  // (the settings file can't hold it: not taken)
    idle();
    down();
    selected("save.location.reset");  // (enabled now: a folder is set)
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(!strcmp(config_editable().save_location, "/games/MHGU saves"));
    { Config saved = config_editable(); assert(config_restart_required(&saved)); }
    press(ImGuiKey_Home);
    down(11);
    selected("save.location.reset");
    confirm();
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(!config_editable().save_location[0]);
    press(ImGuiKey_Home);
    down(11);  // (Use the default folder: disabled again, skipped)
    selected("restart");
    confirm();
    selected("dialog.cancel");
    back();
    selected("restart");
    confirm();
    down();
    confirm();
    assert(requested == 2);
    back();
    requested = 0;
    opened = false;
    frame();
    baseline = config_editable();
    io.DisplaySize = ImVec2(640, 480);
    baseline.menu_scale = 200;
    baseline.language = 1;
    config_store_loaded(&baseline);
    opened = true;
    idle();
    auto *w = ImGui::FindWindowByName("Settings##main");
    assert(w->Size.x <= io.DisplaySize.x);
    assert(w->ContentSize.x <= w->InnerRect.GetWidth() + 1);
    float top = w->Pos.y;
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);  // (Cheats, Mods, Misc)
    selected("sync");  // (the Hub heading first)
    down(4);  // (lag compensation, village quests, urgent quests, then the HP heading is passed)
    selected("hp.0");
    down(5);
    confirm();
    idle();
    assert(w->Pos.y == top);
    press(ImGuiKey_End);
    selected("reset");
    idle(12);
    assert(w->Scroll.y > 0);
    press(ImGuiKey_GamepadDpadUp);
    selected("hp.add");
    confirm();
    assert(settings_panel_dirty());
    down(5);
    confirm();
    selected("hp.add");
    assert(!settings_panel_dirty());
    press(ImGuiKey_Home);
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    selected("reset");
    confirm();
    selected("dialog.cancel");
    down();
    confirm();
    selected("reset");
    assert(!settings_panel_dirty());
    io.DisplaySize = ImVec2(640, 480);
    idle(12);
    assert(w->Size.x <= 640);
    assert(w->Size.y <= 480);
    assert(w->ContentSize.x <= w->InnerRect.GetWidth() + 1);
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);
    selected("volume");
    down(13);  // (mutebg, show.fps, fps, vsync, prewarm, msaa, fxaa, fxaamode, drawdist, smooth, kerning, boldtext, fsr)
    selected("fsr");
    confirm();
    assert(settings_panel_dirty());
    down();
    selected("fsr.quality");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_editable().fsr == 1);
    assert(config_editable().fsr_quality == FSR_QUALITY);
    Config fsr_saved = config_editable();
    assert(config_restart_required(&fsr_saved));
    opened = false;
    frame();
    g_config.fsr = 1;
    opened = true;
    idle();
    selected("volume");
    assert(!settings_panel_dirty()); // FSR-owned AA rows are disabled, not misleading live toggles.
    idle(20);
    float pinned_bottom = pinned_content_bottom();
    float pinned_top = pinned_content_top();
    press(ImGuiKey_End);
    idle(40);
    selected("reset");
    auto *root = ImGui::FindWindowByName("Settings##main");
    assert(root->Scroll.y > 0);
    assert(pinned_content_bottom() == pinned_bottom); // Guide position does not follow the scroll.
    assert(pinned_content_top() == pinned_top);
    assert(root->NavRectRel[ImGuiNavLayer_Main].Max.y + root->Pos.y <= pinned_bottom + 1);
    press(ImGuiKey_Home);
    idle(40);
    selected("volume");
    assert(pinned_content_bottom() == pinned_bottom);
    assert(pinned_content_top() == pinned_top);
    assert(root->NavRectRel[ImGuiNavLayer_Main].Min.y + root->Pos.y >= pinned_top - 1);
    press(ImGuiKey_End);
    idle(20);
    float font = root->FontRefSize;
    io.AddMousePosEvent(root->Pos.x + ImGui::GetStyle().WindowPadding.x + font * 2.6f,
                        root->Pos.y + ImGui::GetStyle().WindowPadding.y + font * .8f);
    idle();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    idle(8);
    selected("display.mode"); // Pinned tabs remain clickable when the option list is scrolled.
    press(ImGuiKey_GamepadR1);
    selected("volume");
    opened = false;
    frame();
    assert(!settings_panel_dirty());
    ImGui::DestroyContext();
    remove("obj/settings-test/panel-settings.ini");
    puts("settings panel: inline numbers, explicit Save/Reset, Back guide, dirty discard, focus and layout "
         "OK");
}
