// Controller navigation exercised against the real ImGui frame loop.
#include "settings_panel.h"
#include "config.h"
#include "app_actions.h"
#include "overlay.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
static int requested;
extern "C" void ui_request_action(bool restart) { requested = restart ? 2 : 1; }
extern "C" bool ui_action_failed(void) { return false; }
extern "C" int overlay_face_position(int semantic) {
    const int p[] = {1, 0, 3, 2};
    return p[semantic];
}
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
static ImGuiID id(const char *name) {
    auto *w = ImGui::FindWindowByName("Settings##main");
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
int main() {
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "obj/settings-test/panel-settings.ini");
#else
    setenv("MHGU_CONFIG", "obj/settings-test/panel-settings.ini", 1);
#endif
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    Config baseline = config_defaults();
    config_store_loaded(&baseline);
    config_runtime_init(&baseline);

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
    selected("fps");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    idle();
    tab();
    selected("face");
    down();
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
    down();
    selected("menu.confirm");
    press(ImGuiKey_GamepadDpadLeft);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_ORIGINAL);
    tab();
    selected("port");
    press(ImGuiKey_GamepadL1);
    selected("face");
    tab();
    tab();
    selected("hp.0");
    tab();
    selected("language");
    tab();
    selected("display.mode");
    tab();
    down(5);  // (fps, vsync, prewarm, msaa, fxaa)
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
    press(ImGuiKey_GamepadFaceUp);
    assert(settings_panel_dirty());
    assert(config_editable().volume == 100);
    press(ImGuiKey_End);
    selected("reset");
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(!settings_panel_dirty());
    assert(config_editable().volume == 99);
    press(ImGuiKey_Home);
    down(5);
    selected("volume");
    press(ImGuiKey_GamepadDpadRight);
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    assert(config_editable().volume == 99);
    frame();
    opened = true;
    idle();
    selected("fps");
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
    selected("language");
    down(6);
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
    selected("fps");
    down(6);
    selected("fsr");
    confirm();
    assert(settings_panel_dirty());
    down();
    selected("fsr.provider");
    press(ImGuiKey_GamepadDpadRight);
    down();
    selected("fsr.quality");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_editable().fsr == 1 && config_editable().fsr_upscaler == FSR_UP_4);
    assert(config_editable().fsr_quality == FSR_QUALITY);
    Config fsr_saved = config_editable();
    assert(config_restart_required(&fsr_saved));
    opened = false;
    frame();
    g_config.fsr = 1;
    opened = true;
    idle();
    selected("fps");
    down(3);
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
    selected("fps");
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
    selected("fps");
    opened = false;
    frame();
    assert(!settings_panel_dirty());
    ImGui::DestroyContext();
    remove("obj/settings-test/panel-settings.ini");
    puts("settings panel: inline numbers, explicit Save/Reset, Back guide, dirty discard, focus and layout "
         "OK");
}
