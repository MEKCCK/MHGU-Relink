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
struct IconFixture { int choice = 0; std::string hash; };
static IconFixture icon_choices[TOUCH_ICON_BUTTON_COUNT];
static uint64_t icon_generation = 1;
static bool icon_save_ok = true, icon_picker_ok = true, icon_picker_pending;
static int icon_picker_result, icon_picker_requests, icon_save_calls;
static std::string icon_picker_hash;
extern "C" bool ui_request_touch_icon(void) {
    ++icon_picker_requests;
    if (!icon_picker_ok || icon_picker_pending) return false;
    return icon_picker_pending = true;
}
extern "C" int ui_take_touch_icon(char hash[65]) {
    if (!icon_picker_result) return 0;
    int result = icon_picker_result;
    icon_picker_result = 0;
    icon_picker_pending = false;
    SDL_strlcpy(hash, icon_picker_hash.c_str(), 65);
    return result;
}
extern "C" const char *touch_icon_button_name(int button) {
    static const char *const names[] = {"Heavy attack  A", "Light attack  X", "Dodge  B", "Sheathe  Y", "Aim  R",
        "Camera  L", "Target  ZL", "Special  ZR", "Menu  +", "Minus  -", "Left stick click", "Right stick click",
        "D-pad", "D-pad visibility", "Movement", "Settings"};
    assert(button >= 0 && button < TOUCH_ICON_BUTTON_COUNT);
    return button < 16 ? names[button] : button == 22 ? "Next ammo" : button == 23 ? "Previous ammo" :
           button == 24 ? "Lock large monster" : "Combined button";
}
extern "C" int touch_icon_builtin_count(void) { return 15; }
extern "C" const char *touch_icon_builtin_name(int choice) {
    static const char *const names[] = {"Sword", "Dodge", "Shield", "Burst", "Target", "Camera", "Menu", "Map",
        "Settings", "Sheathe", "Run", "Crosshair", "Shot", "Bow", "Lock"};
    return names[choice - 1];
}
extern "C" uint64_t touch_layout_generation(void) { return icon_generation; }
extern "C" bool touch_icon_get(int button, int *choice, char hash[65]) {
    assert(button >= 0 && button < TOUCH_ICON_BUTTON_COUNT);
    if (choice) *choice = icon_choices[button].choice;
    if (hash) SDL_strlcpy(hash, icon_choices[button].hash.c_str(), 65);
    return true;
}
extern "C" bool touch_icon_set(int button, int choice, const char *hash, uint64_t expected_generation) {
    ++icon_save_calls;
    if (!icon_save_ok || expected_generation != icon_generation) return false;
    icon_choices[button] = {choice, hash ? hash : ""};
    ++icon_generation;
    return true;
}
struct TemplateFixture { std::string name; Config combo; };
static TemplateFixture templates[TOUCH_TEMPLATE_COUNT];
static int template_info_queries, edit_target = -1, edit_cancelled;
static bool template_use_ok = true, template_save_ok = true;
static std::vector<int> template_uses, template_saves;
static std::vector<Config> template_save_settings;
extern "C" bool touch_template_info(int slot, char *name, int name_size) {
    ++template_info_queries;
    assert(slot >= 0 && slot < TOUCH_TEMPLATE_COUNT);
    if (name && name_size > 0) *name = 0;
    if (templates[slot].name.empty()) return false;
    SDL_utf8strlcpy(name, templates[slot].name.c_str(), name_size);
    return true;
}
extern "C" bool touch_template_save(int slot, const char *name) {
    assert(slot >= 0 && slot < TOUCH_TEMPLATE_COUNT);
    template_saves.push_back(slot);
    template_save_settings.push_back(config_editable());
    if (!template_save_ok) return false;
    templates[slot] = {name, config_editable()};
    return true;
}
extern "C" bool touch_template_use(int slot) {
    assert(slot >= 0 && slot < TOUCH_TEMPLATE_COUNT);
    template_uses.push_back(slot);
    if (!template_use_ok || templates[slot].name.empty()) return false;
    Config next = config_editable();
    const Config &from = templates[slot].combo;
    memcpy(next.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE], from.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE],
           sizeof next.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE]);
    memcpy(next.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE], from.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE],
           sizeof next.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE]);
    memcpy(next.touch_combo_name, from.touch_combo_name, sizeof next.touch_combo_name);
    char error[256];
    bool ok = config_save(&next, error, sizeof error);
    if (ok) ++icon_generation;
    return ok;
}
extern "C" void touch_edit_begin(void) { edit_target = -2; }
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
static void forge_menu_regression() {
    idle();
    for (int page = 0; page < 5; ++page) tab();
    assert(!settings_panel_take_forge());
    seek_row("mods.forge");  // No plugin titles or callbacks exist in this test.
    confirm();
    assert(settings_panel_take_forge());
    assert(!settings_panel_take_forge());
    assert(opened && !settings_panel_dirty());
    click_focused_row();
    assert(settings_panel_take_forge());
    assert(!settings_panel_take_forge());
    opened = false;
    frame();
    puts("PASS: Forge menu is reachable with zero plugins, by controller and pointer, without changing settings");
}
static void open_templates() {
    seek_row("touch.templates");
    confirm();
    assert(opened && !settings_panel_confirming());
}
static void enter_text(const char *row, const char *value) {
    seek_row(row);
    confirm();
    seek_row("key.clear");
    confirm();
    ImGui::GetIO().AddInputCharactersUTF8(value);
    idle();
    seek_row("key.accept");
    confirm();
    assert(focused_row(row));
}
static void assert_combo_snapshot(const Config &expected) {
    const Config actual = config_editable();
    assert(!memcmp(actual.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE],
                   expected.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE],
                   sizeof actual.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE]));
    assert(!memcmp(actual.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE],
                   expected.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE],
                   sizeof actual.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE]));
    assert(!memcmp(actual.touch_combo_name, expected.touch_combo_name, sizeof actual.touch_combo_name));
}
static void template_regression() {
    static_assert(TOUCH_TEMPLATE_COUNT == 40, "The shared layout has forty independent snapshots");
    Config fixture = config_editable();
    for (int slot : {0, 19}) {
        templates[slot].name = slot ? "远程连招" : "近战通用";
        templates[slot].combo = fixture;
        for (int i = 0; i < TOUCH_COMBOS; i++) {
            strcpy(templates[slot].combo.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][i], slot ? "R+A" : "X+A");
            templates[slot].combo.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE][i] = slot ? PADB_R3 : PADB_L3;
            snprintf(templates[slot].combo.touch_combo_name[i], TOUCH_COMBO_NAME_MAX,
                     slot ? "蓄力组合%d" : "近战组合%d", i + 1);
        }
    }
    idle();
    tab();
    tab();
    assert(!settings_panel_dirty());
    open_templates();
    assert(template_info_queries >= TOUCH_TEMPLATE_COUNT);
    const int queries = template_info_queries;
    idle(20);
    assert(template_info_queries == queries); // No disk queries on every rendered frame.
    for (int slot = 0; slot < TOUCH_TEMPLATE_COUNT; slot++) {
        std::string row = "tp." + std::to_string(slot);
        seek_row(row.c_str());
        if (slot % 2) click_focused_row(); else confirm();
        assert(template_uses.empty() && template_saves.empty());
        assert(focused_row("tp.save"));
        back();
        assert(focused_row(row.c_str()));
    }
    seek_row("tp.1");
    confirm();
    enter_text("tp.name", "我的中文模板");
    seek_row("tp.save");
    confirm();
    assert(template_saves.size() == 1 && template_saves.back() == 1);
    assert(templates[1].name == "我的中文模板" && !settings_panel_dirty());
    seek_row("tp.save");
    confirm();
    assert(settings_panel_confirming() && template_saves.size() == 1);
    confirm(); // Cancel is the safe initial choice for an existing template.
    assert(!settings_panel_confirming() && template_saves.size() == 1);
    confirm();
    assert(settings_panel_confirming());
    back();
    assert(!settings_panel_confirming() && template_saves.size() == 1);
    confirm();
    seek_row("dialog.apply");
    confirm();
    assert(template_saves.size() == 2 && template_saves.back() == 1);
    assert(templates[0].name == "近战通用" && templates[19].name == "远程连招");

    back();
    seek_row("tp.0");
    confirm();
    seek_row("tp.use");
    click_focused_row();
    assert(template_uses.size() == 1 && template_uses.back() == 0);
    assert_combo_snapshot(templates[0].combo);
    assert(!settings_panel_dirty()); // Applying must replace saved and draft together.
    back();
    back();
    seek_row("touch.hud");
    confirm();
    const int original_hud = config_editable().touch_hud;
    assert(settings_panel_dirty());
    open_templates();
    seek_row("tp.19");
    confirm();
    seek_row("tp.use");
    confirm();
    assert(settings_panel_confirming() && template_uses.size() == 1);
    confirm();
    assert(!settings_panel_confirming() && settings_panel_dirty() && template_uses.size() == 1);
    confirm();
    back();
    assert(!settings_panel_confirming() && template_uses.size() == 1);
    confirm();
    seek_row("dialog.discard");
    confirm();
    assert(template_uses.size() == 2 && template_uses.back() == 19);
    assert(config_editable().touch_hud == original_hud && !settings_panel_dirty());
    assert_combo_snapshot(templates[19].combo);

    back();
    back();
    seek_row("touch.hud");
    confirm();
    open_templates();
    seek_row("tp.0");
    confirm();
    seek_row("tp.use");
    confirm();
    seek_row("dialog.save");
    confirm();
    assert(template_uses.size() == 3 && template_uses.back() == 0);
    assert(config_editable().touch_hud == !original_hud && !settings_panel_dirty());
    assert_combo_snapshot(templates[0].combo);
    // Saving an unsaved settings edit resolves the draft before taking the snapshot.
    back();
    back();
    seek_row("touch.hud");
    confirm();
    seek_row("touch.buttons");
    confirm();
    seek_row("tc.0");
    confirm();
    enter_text("ce.name", "保存前新连招");
    seek_row("ce.1");
    confirm();
    seek_row("ce.pad");
    confirm();
    seek_row("choice.3");
    confirm();
    back();
    back();
    open_templates();
    seek_row("tp.2");
    confirm();
    enter_text("tp.name", "保留设置");
    seek_row("tp.save");
    confirm();
    assert(settings_panel_confirming());
    seek_row("dialog.save");
    confirm();
    assert(template_saves.size() == 3 && template_saves.back() == 2);
    assert(template_save_settings.back().touch_hud == original_hud && !settings_panel_dirty());
    assert(!strcmp(template_save_settings.back().touch_combo_name[0], "保存前新连招"));
    assert(config_combo_mask(template_save_settings.back().touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][0]) ==
           config_combo_mask("X+A+B"));
    assert(template_save_settings.back().touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE][0] == PADB_MINUS);
    assert_combo_snapshot(templates[2].combo);

    // Failed template operations leave the current common configuration intact.
    const Config before_failure = config_editable();
    template_use_ok = false;
    back();
    seek_row("tp.19");
    confirm();
    seek_row("tp.use");
    confirm();
    assert(template_uses.size() == 4 && template_uses.back() == 19);
    assert(!settings_panel_dirty() && !settings_panel_confirming());
    assert_combo_snapshot(before_failure);
    template_use_ok = true;
    template_save_ok = false;
    back();
    seek_row("tp.3");
    confirm();
    enter_text("tp.name", "写入失败");
    seek_row("tp.save");
    confirm();
    assert(template_saves.size() == 4 && templates[3].name.empty());
    template_save_ok = true;
    // Failed settings Save must not reach the template use callback.
    back();
    back();
    seek_row("touch.hud");
    confirm();
    open_templates();
    seek_row("tp.0");
    confirm();
    seek_row("tp.use");
    confirm();
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "missing-settings-parent/panel.ini");
#else
    setenv("MHGU_CONFIG", "missing-settings-parent/panel.ini", 1);
#endif
    seek_row("dialog.save");
    confirm();
    assert(settings_panel_confirming() && settings_panel_dirty() && template_uses.size() == 4);
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "obj/settings-test/panel-settings.ini");
#else
    setenv("MHGU_CONFIG", "obj/settings-test/panel-settings.ini", 1);
#endif
    seek_row("dialog.discard");
    confirm();
    assert(template_uses.size() == 5 && !settings_panel_dirty());
    back();
    opened = false;
    frame();
    puts("PASS: real settings-panel ImGui loop: forty independent template slots, Chinese naming, explicit Save/Use, overwrite confirmation, draft Save/Discard/Cancel/Back, failure retention and combo snapshot refresh (touch API stubs; no Android/game execution)");
}
static void empty_template_detail_regression() {
    Config fixture = config_editable();
    fixture.language = 2;
    config_store_loaded(&fixture);
    config_runtime_init(&fixture);
    const size_t before = template_saves.size();
    templates[4].name.clear();
    templates[5].name.clear();
    templates[39].name.clear();
    opened = true;
    idle();
    open_templates();
    seek_row("tp.4");
    confirm();
    assert(focused_row("tp.save")); // Entering an empty slot is immediately ready to save.
    confirm();
    assert(template_saves.size() == before + 1 && template_saves.back() == 4);
    assert(templates[4].name == "模板 5" && !settings_panel_confirming());
    back();
    assert(focused_row("tp.4"));
    seek_row("tp.5");
    confirm();
    assert(focused_row("tp.save"));
    confirm();
    assert(template_saves.size() == before + 2 && template_saves.back() == 5);
    assert(templates[5].name == "模板 6" && templates[4].name == "模板 5");
    back();
    seek_row("tp.39");
    confirm();
    assert(focused_row("tp.save"));
    confirm();
    assert(template_saves.size() == before + 3 && template_saves.back() == 39);
    assert(templates[39].name == "模板 40" && templates[19].name == "远程连招");
    back();
    back();
    opened = false;
    frame();
    fixture.language = 0;
    config_store_loaded(&fixture);
    config_runtime_init(&fixture);
    puts("PASS: empty template opens its own detail page focused on Save; slots 5, 6 and 40 save directly with independent default Chinese names and slot 20 stays unchanged");
}
static void touch_icon_regression() {
    static_assert(TOUCH_ICON_BUTTON_COUNT == 25, "Append ammo and the dedicated lock icon without renumbering existing buttons");
    opened = true;
    idle();
    seek_row("touch.icons");
    confirm();
    for (int button = 0; button < TOUCH_ICON_BUTTON_COUNT; ++button) {
        seek_row(("ti." + std::to_string(button)).c_str());
        confirm();
        assert(focused_row("ti.default"));
        const int choice = button % touch_icon_builtin_count() + 1;
        seek_row(("ti.builtin." + std::to_string(choice)).c_str());
        confirm();
        assert(icon_choices[button].choice == choice && !settings_panel_dirty());
        back();
    }
    for (int button = 0; button < TOUCH_ICON_BUTTON_COUNT; ++button)
        assert(icon_choices[button].choice == button % touch_icon_builtin_count() + 1);
    seek_row("ti.24");
    confirm();
    seek_row("ti.builtin.15");
    confirm();
    assert(icon_choices[24].choice == 15 && icon_choices[23].choice == 9);
    back();
    seek_row("ti.21"); // The sixth combo has its own icon.
    confirm();
    const int saved = icon_choices[21].choice;
    const int neighbor = icon_choices[20].choice;
    icon_save_ok = false;
    seek_row("ti.default");
    confirm();
    assert(icon_choices[21].choice == saved);
    icon_save_ok = true;
    seek_row("ti.import");
    confirm();
    assert(icon_picker_pending);
    icon_picker_result = -1;
    idle();
    assert(icon_choices[21].choice == saved);
    seek_row("ti.import");
    confirm();
    icon_picker_result = -2;
    idle();
    assert(icon_choices[21].choice == saved);
    seek_row("ti.import");
    confirm();
    icon_picker_hash = std::string(64, 'a');
    icon_picker_result = 1;
    idle();
    assert(icon_choices[21].choice == -1 && icon_choices[21].hash == icon_picker_hash);
    assert(icon_choices[20].choice == neighbor);
    const int accepted = icon_save_calls;
    seek_row("ti.import");
    confirm();
    ++icon_generation; // A different layout was installed while Android's picker was open.
    icon_picker_result = 1;
    idle();
    assert(icon_save_calls == accepted);
    seek_row("ti.import");
    confirm();
    back();
    seek_row("ti.20");
    confirm();
    icon_picker_result = 1;
    idle();
    assert(icon_save_calls == accepted && icon_choices[20].choice == neighbor);
    seek_row("ti.import");
    confirm();
    opened = false;
    frame();
    opened = true;
    idle();
    seek_row("touch.icons");
    confirm();
    seek_row("ti.20");
    confirm();
    icon_picker_result = 1;
    idle();
    assert(icon_save_calls == accepted); // Reopening the same button cannot revive a closed request.
    icon_picker_ok = false;
    seek_row("ti.import");
    confirm();
    assert(!icon_picker_pending && icon_save_calls == accepted);
    icon_picker_ok = true;
    seek_row("ti.default");
    confirm();
    assert(icon_choices[20].choice == 0 && icon_choices[21].choice == -1);
    back();
    back();
    opened = false;
    frame();
    puts("PASS: all 25 common button icon rows including six combos, two ammo arrows and the dedicated lock key, default/built-in/custom choices, immediate save, failure/cancel retention, layout generation and closed/switched picker rejection (real UI, stub importer)");
}
static void combo_layout_regression() {
    static_assert(TOUCH_UNIVERSAL_PROFILE == 19 && TOUCH_LAYOUT_PROFILES == 20,
                  "The common combo profile must not renumber legacy profiles");
    Config fixture = config_editable();
    for (int profile = 0; profile < TOUCH_UNIVERSAL_PROFILE; profile++) {
        strcpy(fixture.touch_weapon_combo[profile][0], "L+ZL");
        fixture.touch_weapon_combo_pad[profile][0] = PADB_L3;
    }
    memset(fixture.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE], 0, sizeof fixture.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE]);
    memset(fixture.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE], 0, sizeof fixture.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE]);
    memset(fixture.touch_combo_name, 0, sizeof fixture.touch_combo_name);
    config_store_loaded(&fixture);
    config_runtime_init(&fixture);
    config_set_touch_profile(17); // Legacy runtime selections must never redirect the common editor.
    opened = true;
    idle();
    seek_row("touch.buttons");
    confirm();
    for (int k = 0; k < TOUCH_COMBOS; k++) {
        const std::string row = "tc." + std::to_string(k);
        seek_row(row.c_str());
        confirm();
        press(ImGuiKey_Home);
        assert(focused_row("ce.name")); // No weapon/layout selector remains.
        const std::string name = "中文连招" + std::to_string(k + 1);
        enter_text("ce.name", name.c_str());
        seek_row("ce.0");
        confirm();
        seek_row("ce.2");
        confirm();
        seek_row("ce.pad");
        confirm();
        const std::string pad = "choice." + std::to_string(k + 1);
        seek_row(pad.c_str());
        confirm();
        back();
    }
    back();
    seek_row("save");
    confirm();
    assert(!settings_panel_dirty());
    const Config saved = config_editable();
    for (int k = 0; k < TOUCH_COMBOS; k++) {
        assert(config_combo_mask(saved.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][k]) == config_combo_mask("X+A"));
        assert(saved.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE][k] == k + 1);
        const std::string name = "中文连招" + std::to_string(k + 1);
        assert(name == saved.touch_combo_name[k]);
        char published[TOUCH_COMBO_NAME_MAX];
        config_live_touch_combo_name(k, published, sizeof published);
        assert(name == published);
    }
    assert(!memcmp(saved.touch_weapon_combo, fixture.touch_weapon_combo,
                   TOUCH_UNIVERSAL_PROFILE * sizeof saved.touch_weapon_combo[0]));
    assert(!memcmp(saved.touch_weapon_combo_pad, fixture.touch_weapon_combo_pad,
                   TOUCH_UNIVERSAL_PROFILE * sizeof saved.touch_weapon_combo_pad[0]));
    assert(!memcmp(saved.touch_combo, fixture.touch_combo, sizeof saved.touch_combo));
    assert(!memcmp(saved.touch_combo_pad, fixture.touch_combo_pad, sizeof saved.touch_combo_pad));
    // Request all sixteen buttons through the real editor: over-capacity choices must not truncate a token.
    seek_row("touch.buttons");
    confirm();
    seek_row("tc.0");
    confirm();
    static const int order[16] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 13, 15, 12, 14, 4, 5};
    uint32_t expected = config_combo_mask("X+A");
    int refused = 0;
    for (int n = 0; n < 16; n++) {
        const std::string row = "ce." + std::to_string(n);
        seek_row(row.c_str());
        char candidate[128];
        config_combo_text(expected | (1u << order[n]), candidate, sizeof candidate);
        if (strlen(candidate) < sizeof saved.touch_weapon_combo[0][0]) expected |= 1u << order[n];
        else ++refused;
        press(ImGuiKey_GamepadDpadRight);
    }
    assert(refused > 0);
    back();
    back();
    seek_row("save");
    confirm();
    const Config bounded = config_editable();
    assert(config_combo_mask(bounded.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][0]) == expected);
    assert(!strcmp(bounded.touch_combo_name[0], saved.touch_combo_name[0]));
    assert(!settings_panel_dirty());
    opened = false;
    frame();
    puts("PASS: all six common combos accept Chinese names and independent controller mappings through the real UI; legacy weapon/layout bindings remain unchanged and there is no weapon selector");
}
static void touch_button_rows_regression() {
    assert(config_defaults().touch_show[TB_AMMO] == 1);
    for (int legacy_aim = 0; legacy_aim < 2; legacy_aim++) {
        Config fixture = config_editable();
        fixture.touch_show[TB_AIM] = legacy_aim;
        fixture.touch_show[TB_GUARD] = legacy_aim;
        fixture.touch_show[TB_AMMO] = !legacy_aim;
        const std::string legacy = std::string("ShowAim = ") + (fixture.touch_show[TB_AIM] ? "On\n" : "Off\n");
        const std::string ammo = std::string("ShowAmmo = ") + (fixture.touch_show[TB_AMMO] ? "On\n" : "Off\n");
        std::ofstream("obj/settings-test/panel-settings.ini", std::ios::binary) << "[Touch]\n" << legacy << ammo;
        config_store_loaded(&fixture);
        config_runtime_init(&fixture);
        opened = true;
        idle();
        seek_row("touch.buttons");
        confirm();
        seek_row("tb.0");
        for (int i = 0; i < TB_COUNT; i++) {
            if (i == TB_RANGED || i == TB_FIRE || i == TB_SECOND || i == TB_AIM ||
                i == TB_RSPECIAL || i == TB_MENU_ZL || i == TB_MENU_ZR) continue;
            const std::string row = "tb." + std::to_string(i);
            assert(focused_row(row.c_str()));
            down();
        }
        assert(focused_row("tc.0") && !settings_panel_dirty());
        const std::string guard = "tb." + std::to_string(TB_GUARD);
        seek_row(guard.c_str());
        confirm();
        assert(settings_panel_dirty());
        seek_row(("tb." + std::to_string(TB_AMMO)).c_str());
        confirm();
        assert(config_editable().touch_show[TB_AMMO] == fixture.touch_show[TB_AMMO]);
        back();
        seek_row("save");
        confirm();
        assert(!settings_panel_dirty());
        const Config saved = config_editable();
        for (int i = 0; i < TB_COUNT; i++)
            assert(saved.touch_show[i] == (i == TB_GUARD || i == TB_AMMO ? !fixture.touch_show[i] : fixture.touch_show[i]));
        std::ifstream in("obj/settings-test/panel-settings.ini", std::ios::binary);
        const std::string ini((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(ini.find(legacy) != std::string::npos);
        assert(ini.find(saved.touch_show[TB_AMMO] ? "ShowAmmo = On" : "ShowAmmo = Off") != std::string::npos);
        opened = false;
        frame();
    }
    puts("PASS: common touch-button rows retain Aim (R), native item arrows and independent ammo arrows; ShowAmmo defaults On and saves both On/Off independently, while retired ranged controls stay hidden and their settings unchanged");
}
static void removed_motion_rows_regression() {
    opened = true;
    idle();
    seek_row("touch.auto_r");
    down();
    assert(focused_row("touch.buttons"));
    down();
    assert(focused_row("touch.camera") && !settings_panel_dirty());
    opened = false;
    frame();
    puts("PASS: Controls moves directly from automatic running to screen buttons and touch-camera speed, with no motion controls or sensitivity/inversion rows");
}
static void auto_r_regression() {
    char error[256];
    Config fixture = config_editable();
    fixture.touch_auto_r = config_defaults().touch_auto_r;
    fixture.touch_floating = 1;
    fixture.touch_show[TB_GUARD] = 0;
    assert(!fixture.touch_auto_r);
    assert(config_save(&fixture, error, sizeof error));
    auto saved_choice = [&](int enabled) {
        const Config saved = config_editable();
        assert(saved.touch_auto_r == enabled && config_live_touch_auto_r() == enabled);
        assert(saved.touch_floating == fixture.touch_floating);
        assert(!memcmp(saved.touch_show, fixture.touch_show, sizeof saved.touch_show));
        assert(!memcmp(saved.binds, fixture.binds, sizeof saved.binds));
        std::ifstream in("obj/settings-test/panel-settings.ini", std::ios::binary);
        const std::string ini((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(ini.find(enabled ? "AutoR = On" : "AutoR = Off") != std::string::npos);
    };
    opened = true;
    idle();
    seek_row("touch.floating");
    down();
    assert(focused_row("touch.auto_r"));
    assert(!settings_panel_dirty());
    confirm();
    assert(settings_panel_dirty());
    saved_choice(0); // The toggle edits the draft; only Save publishes it to live input.
    seek_row("save");
    confirm();
    assert(!settings_panel_dirty());
    saved_choice(1);

    seek_row("touch.auto_r");
    confirm();
    assert(settings_panel_dirty());
    back();
    assert(settings_panel_confirming() && focused_row("dialog.cancel"));
    confirm();
    assert(opened && !settings_panel_confirming() && settings_panel_dirty());
    saved_choice(1);
    back();
    seek_row("dialog.discard");
    confirm();
    assert(!opened && !settings_panel_dirty());
    saved_choice(1);
    frame();
    opened = true;
    idle();
    seek_row("touch.auto_r");
    confirm();
    seek_row("save");
    confirm();
    assert(!settings_panel_dirty());
    saved_choice(0);

    // Reset Controls restores Off in the draft and publishes it only when saved.
    const Config before_reset = config_editable();
    seek_row("touch.auto_r");
    confirm();
    seek_row("save");
    confirm();
    saved_choice(1);
    seek_row("reset");
    confirm();
    seek_row("dialog.reset");
    confirm();
    assert(settings_panel_dirty() && config_live_touch_auto_r());
    seek_row("save");
    confirm();
    assert(!settings_panel_dirty());
    assert(!config_editable().touch_auto_r && !config_live_touch_auto_r());
    opened = false;
    frame();
    assert(config_save(&before_reset, error, sizeof error)); // Retain prior combo fixtures for the previews.
    saved_choice(0);
    puts("PASS: automatic-running switch (AutoR) follows Floating left stick; draft, Save, Cancel, Discard, persisted On/Off and Reset Controls work without changing manual R or floating-stick choices");
}
// Capture the actual phone-style settings draw lists on the host, not a mockup or an Android/game screenshot.
static void template_previews() {
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
    for (int slot = 0; slot < TOUCH_TEMPLATE_COUNT; slot++) {
        templates[slot].name = "自定义模板" + std::to_string(slot + 1);
        templates[slot].combo = translated;
    }
    templates[0].name = "近战通用";
    templates[19].name = "远程连招";
    open_templates();
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
    capture("settings-common-templates-zh-640x360.png");
    seek_row("tp.0");
    confirm();
    seek_row("tp.use");
    capture("settings-template-name-zh-640x360.png");
    back();
    templates[3].name.clear();
    back();
    open_templates();
    seek_row("tp.3");
    confirm();
    capture("settings-empty-template-detail-zh-640x360.png");
    back();
    back();
    seek_row("touch.icons");
    confirm();
    seek_row("ti.21");
    capture("settings-touch-icons-list-zh-640x360.png");
    confirm();
    capture("settings-touch-icon-picker-zh-640x360.png");
    back();
    seek_row("ti.22");
    capture("settings-ammo-icons-list-zh-640x360.png");
    seek_row("ti.23");
    capture("settings-ammo-previous-icon-zh-640x360.png");
    confirm();
    capture("settings-ammo-icon-picker-zh-640x360.png");
    back();
    back();
    seek_row("touch.auto_r");
    capture("settings-stick-run-off-zh-640x360.png");
    seek_row("touch.buttons");
    capture("settings-arrow-help-zh-640x360.png");
    confirm();
    seek_row(("tb." + std::to_string(TB_GUARD)).c_str());
    capture("settings-aim-r-zh-640x360.png");
    seek_row(("tb." + std::to_string(TB_AMMO)).c_str());
    capture("settings-ammo-arrows-zh-640x360.png");
    seek_row(("tb." + std::to_string(TB_ITEM_ARROWS)).c_str());
    capture("settings-item-arrows-zh-640x360.png");
    seek_row("tc.5");
    capture("settings-combo-names-zh-640x360.png");
    seek_row("tc.0");
    confirm();
    seek_row("ce.name");
    capture("settings-combo-editor-zh-640x360.png");
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
    const bool touch_presets_only = argc > 1 && (!strcmp(argv[1], "--touch-templates") || !strcmp(argv[1], "--touch-presets"));
    const bool forge_menu_only = argc > 1 && !strcmp(argv[1], "--forge-menu");
    if (touch_presets_only || forge_menu_only) setvbuf(stdout, nullptr, _IONBF, 0);
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
    if (touch_presets_only || forge_menu_only) {
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefaultVector();
    } else {
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
    }
    Config baseline = config_defaults();
    config_store_loaded(&baseline);
    config_runtime_init(&baseline);

    if (forge_menu_only) {
        forge_menu_regression();
        ImGui::DestroyContext();
        remove("obj/settings-test/panel-settings.ini");
        return 0;
    }
    if (touch_presets_only) {
        template_regression();
        empty_template_detail_regression();
        combo_layout_regression();
        touch_button_rows_regression();
        auto_r_regression();
        removed_motion_rows_regression();
        touch_icon_regression();
        template_previews();
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
    seek_row("fps");
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
    seek_row("fps");
    confirm();
    selected("choice.2");  // (91 is none of them: the one below)
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_editable().fps == 60);
    seek_row("fps");
    confirm();
    selected("choice.1");
    back();  // (Back closes the dropdown, nothing changed)
    selected("fps");
    assert(!settings_panel_dirty() && opened);
    tab();
    selected("face");
    // The Keybinds submenu: a key chosen for B, then a mouse button as its second; the recomp's own keys refused
    seek_row("keybinds");
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
    seek_row("keybinds");
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
    down(5);  // (lag compensation, village quests, urgent quests, rewards, then the HP heading is passed)
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
    selected("cheat.1");  // (the Cheats tab: the master code is always on, the cheat after it takes the focus)
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
    seek_row("menu.confirm");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_SOUTH);
    assert(config_editable().face_buttons == FACE_POSITION);
    assert(!config_restart_required(&baseline));
    seek_row("menu.confirm");
    press(ImGuiKey_GamepadDpadLeft);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_ORIGINAL);
    press(ImGuiKey_Home);
    selected("language");
    // Save folder (PC): Choose folder opens the system's picker, whose folder lands in the draft; Save keeps it
    seek_row("bonus");
    seek_row("save.location");
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
    seek_row("save.location.reset");
    confirm();
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(!config_editable().save_location[0]);
    seek_row("restart");  // (Use the default folder: disabled again, skipped)
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
    // Pinned-guide scrolling at a normal size: Home brings the focused row back under the pinned tabs. (The 200%
    // layout in a 640x480 window below collapses the body clip, where no scroll can make a row visible.)
    {
        auto *root = ImGui::FindWindowByName("Settings##main");
        press(ImGuiKey_End);
        idle(40);
        float pinned_top = pinned_content_top(), pinned_bottom = pinned_content_bottom();
        assert(root->NavRectRel[ImGuiNavLayer_Main].Max.y + root->Pos.y <= pinned_bottom + 1);
        press(ImGuiKey_Home);
        idle(40);
        assert(root->NavRectRel[ImGuiNavLayer_Main].Min.y + root->Pos.y >= pinned_top - 1);
        assert(pinned_content_top() == pinned_top && pinned_content_bottom() == pinned_bottom);
    }
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
    down(5);  // (lag compensation, village quests, urgent quests, rewards, then the HP heading is passed)
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
    seek_row("fsr");
    confirm();
    assert(settings_panel_dirty());
    seek_row("fsr.quality");
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
    float end_scroll = root->Scroll.y;
    press(ImGuiKey_Home);
    idle(40);
    selected("volume");
    assert(pinned_content_bottom() == pinned_bottom);
    assert(pinned_content_top() == pinned_top);
    assert(root->Scroll.y < end_scroll);  // (Home scrolls the first row back up under the pinned tabs)
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
