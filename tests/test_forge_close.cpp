// Real overlay/ImGui frames with no Vulkan or game data. Run test_forge_close.py.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
// SDL and the C++ library keep their host ABI; only overlay.cpp uses its phone UI.
#define __ANDROID__ 1
#define MHGU_NULL_BACKEND 1
#include "../runtime/overlay.cpp"
#undef __ANDROID__

static uint64_t test_ticks = 1000000000;
static bool want_forge;
static int settings_draws;
static int settings_cancel_events;
static ImRect close_rect;
static ImRect plugin_header_rect;
static bool close_seen;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext *, ImGuiID, const ImRect &, const ImGuiLastItemData *) {}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *ctx, ImGuiID, const char *label, ImGuiItemStatusFlags) {
    if (!strcmp(label, settings_text(config_language(), "Close Forge"))) {
        close_rect = ctx->LastItemData.Rect;
        close_seen = true;
    }
    if (!strcmp(label, "Plugin UI")) plugin_header_rect = ctx->LastItemData.Rect;
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID) { return nullptr; }

extern "C" {
Config g_config = {};
volatile uint64_t g_present_count;
const unsigned char _binary_font_ttf_start[] = {0};
const unsigned char _binary_font_ttf_end[] = {0};
SDL_Window *ui_window(void) { return nullptr; }
void vk_lock(void) {}
void vk_unlock(void) {}
void vi_pause_game(void) {}
bool shader_prewarm_progress(uint64_t *, uint64_t *) { return false; }
bool romfs_mods_progress(int *, int *) { return false; }
bool forge_plugins_have(int) { return false; }
int forge_plugin_titles(const char **, int) { return 0; }
void forge_imgui_init(void *) {}
void forge_imgui_run(void *, int which) {
    if (which == FORGE_IMGUI_RENDER)
        for (int row = 0; row < 80; ++row) ImGui::Text("Plugin option %d", row);
}
bool chat_composing(char *, size_t) { return false; }
void chat_position(float *, float *) {}
void chat_set_box(float, float, float, float) {}
bool perfstats_visible(void) { return false; }
void perfstats_draw(float, float) {}
bool touch_drawn(void) { return false; }
void touch_draw(float, float) {}
int config_language(void) { return 0; }
int config_live_menu_confirm(void) { return MENU_CONFIRM_ORIGINAL; }
int config_show_battery(void) { return BATTERY_OFF; }
bool config_show_fps(void) { return false; }
Uint64 SDL_GetTicksNS(void) { return test_ticks; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *) { return 0; }
float SDL_GetWindowPixelDensity(SDL_Window *) { return 1; }
const char *SDL_GetScancodeName(SDL_Scancode) { return "test"; }
SDL_PowerState SDL_GetPowerInfo(int *, int *) { return SDL_POWERSTATE_NO_BATTERY; }
size_t SDL_strlcpy(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) { size_t count = std::min(len, n - 1); memcpy(dst, src, count); dst[count] = 0; }
    return len;
}
}

// Settings are not under test: keep only its request/visibility contract. The
// actual Forge opening, event routing, drawing and set_open transition are real.
void settings_panel(bool *open) {
    if (*open) {
        ++settings_draws;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ++settings_cancel_events;
    }
}
void settings_panel_request_close(void) {}
bool settings_panel_confirming(void) { return false; }
void settings_panel_before_frame(bool) {}
void settings_panel_request_back(void) {}
bool settings_panel_take_forge(void) { bool out = want_forge; want_forge = false; return out; }
bool settings_panel_dirty(void) { return false; }
int settings_panel_theme(void) { return 0; }
bool settings_panel_key(int) { return false; }
bool settings_panel_mouse(int) { return false; }
bool settings_panel_wheel(int) { return false; }

static void frame() {
    close_seen = false;
    test_ticks += 16666667;
    overlay_frame(nullptr);
}
static void key(SDL_Scancode code, bool down) {
    SDL_Event e = {};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = code;
    assert(overlay_event(&e));
    frame();
}
static void tap(const ImVec2 &p) {
    SDL_Event e = {};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.which = SDL_TOUCH_MOUSEID; e.motion.x = p.x; e.motion.y = p.y;
    assert(overlay_event(&e)); frame();
    e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.which = SDL_TOUCH_MOUSEID; e.button.button = SDL_BUTTON_LEFT;
    e.button.x = p.x; e.button.y = p.y;
    assert(overlay_event(&e)); frame();
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    assert(overlay_event(&e)); frame();
}
static void open_from_settings() {
    if (!g_settings_open) overlay_toggle_settings();
    want_forge = true; frame(); frame();
    assert(g_forge_open && g_forge_over && g_settings_open && overlay_menu_open());
    assert(close_seen);
}
static void expect_settings_restored() {
    assert(!g_forge_open && !g_forge_over && g_settings_open && overlay_menu_open());
    int before = settings_draws; frame();
    assert(settings_draws == before + 1);
}

int main() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.Fonts->AddFontDefaultVector(); io.Fonts->Build(); io.Fonts->SetTexID(1);
    forge_style(g_base_style); g_base_style.FontSizeBase = 12; g_scale = 0;
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
    g_enabled = g_ready = true; g_menu_key = SDL_SCANCODE_INSERT;
    // Small landscape viewport: the real button must fit and take a finger tap.
    g_disp_w = 640; g_disp_h = 360;
    open_from_settings();
    assert(close_rect.GetHeight() >= 48);
    assert(close_rect.GetWidth() >= 300);
    assert(close_rect.Min.x >= 0 && close_rect.Max.x <= 640);
    assert(close_rect.Min.y >= 0 && close_rect.Max.y <= 360);
    ImVec2 button_position = close_rect.Min;
    tap(plugin_header_rect.GetCenter());
    assert(g_forge_open);
    ImGuiWindow *content = ImGui::FindWindowByName("forge/##forge_contents");
    if (!content) {
        ImGuiWindow *root = ImGui::FindWindowByName("forge");
        for (ImGuiWindow *child : root->DC.ChildWindows)
            if (child->ChildId == root->GetID("##forge_contents")) content = child;
    }
    assert(content);
    frame();
    assert(content->ScrollMax.y > 100);
    ImGui::SetScrollY(content, 100); frame();
    assert(content->Scroll.y >= 100);
    assert(close_seen && close_rect.Min.x == button_position.x && close_rect.Min.y == button_position.y);
    tap(close_rect.GetCenter()); expect_settings_restored();
    // The host content child must not prevent Escape from cancelling Forge.
    open_from_settings();
    ImGui::FocusWindow(content);
    key(SDL_SCANCODE_ESCAPE, true); expect_settings_restored();
    key(SDL_SCANCODE_ESCAPE, false);
    // The B that dismisses Forge stays held through settings restoration.
    // Real apply_input must suppress it until release, then accept a new press.
    open_from_settings();
    OverlayPad pad = {};
    overlay_pad(&pad); frame(); frame();
    ImGui::FocusWindow(content); frame();
    int cancels_before = settings_cancel_events;
    pad.b = true; overlay_pad(&pad); frame();
    assert(!g_forge_open && !g_forge_over && g_settings_open && overlay_menu_open());
    for (int held_frame = 0; held_frame < 4; ++held_frame) {
        int draws_before = settings_draws;
        overlay_pad(&pad); frame();
        assert(g_settings_open && overlay_menu_open());
        assert(settings_draws == draws_before + 1);
        assert(settings_cancel_events == cancels_before);
        assert(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight));
    }
    pad.b = false; overlay_pad(&pad); frame();
    assert(settings_cancel_events == cancels_before);
    pad.b = true; overlay_pad(&pad); frame();
    assert(settings_cancel_events == cancels_before + 1);
    pad.b = false; overlay_pad(&pad); frame();
    // Back is the reliable exit even when a plugin owns keyboard focus.
    open_from_settings();
    ImGui::SetActiveID(0x123456, ImGui::FindWindowByName("forge"));
    key(SDL_SCANCODE_AC_BACK, true); expect_settings_restored();
    key(SDL_SCANCODE_AC_BACK, false);
    // A standalone Forge menu should return to gameplay rather than create settings.
    g_settings_open = false; set_open(false);
    key(SDL_SCANCODE_INSERT, true); key(SDL_SCANCODE_INSERT, false);
    assert(g_forge_open && !g_settings_open);
    key(SDL_SCANCODE_AC_BACK, true);
    assert(!g_forge_open && !g_forge_over && !overlay_menu_open());
    snap_clear(); ImGui::DestroyContext();
    puts("PASS: real Forge tap, pinned close control, held gamepad B, Android Back and settings restoration");
}
