// Host regression: real SDL finger events and ImGui drawing; no game files or APK execution.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "../runtime/touch.cpp"

static SDL_Window *window;
static Config cfg;
static bool settings_open;
static int drawn, ranged, profile, bow_type, weapon_override = -1;
static constexpr int UNKNOWN_WEAPON = -2;
static bool in_quest = true;
static bool aiming, lock_on, persistent_lock_on, hunt_camera = true;
static int target_cycles;
static uint32_t local_hunter;
static struct {
    uint32_t hunter;
    int type, begins, ends, cancels, resets, adds;
    bool held;
    float dx, dy;
} bow_drag;
static std::vector<int> item_steps;
static int touch_item_pending;
static int screen_w = 1280, screen_h = 720;
extern "C" {
volatile uint64_t g_town_ns;
float g_render_aspect = 16.0f / 9.0f;
float g_ui_anchor_aspect = 16.0f / 9.0f;
uint64_t plat_now_ns(void) { return SDL_GetTicksNS(); }
SDL_Window *ui_window(void) { return window; }
uint32_t weapon_hunter(void) { return local_hunter; }
int weapon_drawn(void) { return drawn; }
int weapon_ranged(uint32_t) { return ranged; }
int weapon_type(void) { return weapon_override == UNKNOWN_WEAPON ? -1 : weapon_override >= 0 ? weapon_override : ranged == WEAPON_BOWGUN ? 4 : ranged == WEAPON_BOW ? 10 : 0; }
int presence_in_quest(void) { return in_quest; }
bool weapon_aiming(uint32_t) { return aiming; }
bool weapon_moving(uint32_t) { return true; }
unsigned weapon_action(uint32_t) { return 0; }
int weapon_bow_type(void) { return bow_type; }
bool mousecam_active(void) { return hunt_camera; }
void mousecam_target_cycle(void) { ++target_cycles; }
void mousecam_bow_drag_begin(uint32_t hunter, int type) {
    assert(hunter && !bow_drag.held);
    bow_drag.hunter=hunter; bow_drag.type=type; bow_drag.held=true;
    bow_drag.dx=bow_drag.dy=0; ++bow_drag.begins;
}
void mousecam_bow_drag_add(float dx, float dy) {
    assert(bow_drag.held && std::isfinite(dx) && std::isfinite(dy));
    bow_drag.dx+=dx; bow_drag.dy+=dy; ++bow_drag.adds;
}
void mousecam_bow_drag_end(bool cancel) {
    assert(bow_drag.held); bow_drag.held=false; ++bow_drag.ends;
    if(cancel) { bow_drag.dx=bow_drag.dy=0; ++bow_drag.cancels; }
}
void mousecam_bow_drag_reset(void) {
    bow_drag.held=false; bow_drag.dx=bow_drag.dy=0; ++bow_drag.resets;
}
void itembar_scroll(int dir) { item_steps.push_back(dir); }
void itembar_touch_scroll(int dir) { item_steps.push_back(dir); touch_item_pending=dir>0?1:-1; }
void itembar_touch_cancel(void) { touch_item_pending=0; }
void haptics_tap(void) {}
bool overlay_menu_open(void) { return settings_open; }
void overlay_toggle_settings(void) { settings_open = !settings_open; }
bool overlay_event(const SDL_Event *) { return true; }
Config config_editable(void) { return cfg; }
int config_language(void) { return cfg.language; }
int config_live_touch_controls(void) { return cfg.touch_controls; }
int config_live_touch_hud(void) { return cfg.touch_hud; }
int config_live_touch_auto_r(void) { return cfg.touch_auto_r; }
int config_live_touch_floating(void) { return cfg.touch_floating; }
int config_live_touch_show(int b) { assert(b >= 0 && b < TB_COUNT); return cfg.touch_show[b]; }
void config_toggle_touch_dpad(void) { cfg.touch_show[TB_DPAD] = !cfg.touch_show[TB_DPAD]; }
int config_live_lock_on(void) { return lock_on; }
int config_live_persistent_lock_on(void) { return persistent_lock_on; }
uint32_t config_live_touch_combo(int p, int i) {
    assert(p >= 0 && p < TOUCH_LAYOUT_PROFILES && i >= 0 && i < TOUCH_COMBOS);
    const char *text = cfg.touch_weapon_combo[p][i];
    if (!strcmp(text, "A")) return B(BTN_A);
    if (!strcmp(text, "X")) return B(BTN_X);
    if (!strcmp(text, "X+A")) return B(BTN_X) | B(BTN_A);
    assert(!*text); return 0;
}
int config_live_touch_profile(void) { return profile; }
void config_set_touch_profile(int value) { profile = value; }
float config_live_touch_camera(void) { return 1; }
float config_live_touch_opacity(void) { return .8f; }
void config_combo_text(uint32_t buttons, char *out, size_t n) {
    const char *text = buttons == (B(BTN_X) | B(BTN_A)) ? "X+A" : buttons == B(BTN_X) ? "X" : buttons == B(BTN_A) ? "A" : "";
    if (n) snprintf(out, n, "%s", text);
}
}
struct Input { uint64_t b{}, touch_b{}; int lx{}, ly{}, rx{}, ry{}; float dx{}, dy{}; };
static Input update(uint64_t controller = 0, bool pad = false, bool typing = false) {
    Input v; v.b = controller;
    v.touch_b = touch_input(pad, typing, &v.b, &v.lx, &v.ly, &v.rx, &v.ry, &v.dx, &v.dy);
    return v;
}
static Input update_axes(uint64_t buttons,int lx,int ly,int rx,int ry) {
    Input v; v.b=buttons; v.lx=lx; v.ly=ly; v.rx=rx; v.ry=ry;
    v.touch_b=touch_input(true,false,&v.b,&v.lx,&v.ly,&v.rx,&v.ry,&v.dx,&v.dy);
    return v;
}
static void event(Uint32 type, SDL_FingerID id, float x, float y) {
    SDL_Event e{}; e.type = type; e.tfinger.fingerID = id; e.tfinger.touchID = 1;
    e.tfinger.windowID = SDL_GetWindowID(window);
    e.tfinger.x = x / screen_w; e.tfinger.y = y / screen_h;
    assert(SDL_PushEvent(&e)); bool dispatched = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == type && e.tfinger.fingerID == id) dispatched = true;
        if (e.type >= SDL_EVENT_FINGER_DOWN && e.type <= SDL_EVENT_FINGER_CANCELED) assert(touch_event(&e));
    }
    assert(dispatched);
}
static void at(Uint32 type, int ctl, SDL_FingerID id = 1, float ux = 0, float uy = 0) {
    Geo g = place(g_s.layout, ctl, (float)screen_w, (float)screen_h);
    event(type, id, g.c.x + ux * g.r, g.c.y + uy * g.r);
}
static void fresh(int w = 1280, int h = 720) {
    screen_w = w; screen_h = h; assert(SDL_SetWindowSize(window, w, h)); SDL_PumpEvents();
    g_render_aspect=(float)w/h;
    g_ui_anchor_aspect=16.0f/9.0f;
    touch_reset(); g_s = {}; g_s.loaded = true; g_s.layout = defaults(0); g_s.saved = g_s.layout;
    g_s.presets_loaded = true;
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) g_s.weapon_layout[p]=p;
    profile = 0; weapon_override = -1; in_quest = true; g_s.in_quest = true; g_s.detected_weapon = 0;
    g_s.touched = true; g_s.w = (float)w; g_s.h = (float)h; g_s.sel = -1;
    cfg = {}; cfg.touch_controls = TOUCH_ON; cfg.touch_auto_r = 1;
    const int shown[] = TOUCH_SHOW_DEFAULT; std::copy(shown, shown + TB_COUNT, cfg.touch_show);
    settings_open = false; drawn = 0; ranged = 0; bow_type = 0; aiming = lock_on = persistent_lock_on = false; hunt_camera = true;
    local_hunter=0; bow_drag={};
    target_cycles = 0; item_steps.clear();
    g_lock_follow = g_lock_shots = false;
    g_visible = false; g_gear_only = false; g_style = g_s.layout.style; g_ranged = 0;
    g_zones = {}; g_build = {}; g_build_menu = false; g_menu_ns = 0; g_press_ns = 0;
    std::fill(std::begin(g_editor_zones),std::end(g_editor_zones),EditorZone{});
    std::fill(std::begin(g_build_editor),std::end(g_build_editor),EditorZone{});
    g_play_ns = SDL_GetTicksNS(); g_window_open = g_window_asked = false; g_town_ns = 0;
    update(); assert(!g_s.menu);
}
static void game_menu_state(bool menu) {
    g_menu_ns = menu ? SDL_GetTicksNS() : 0;
    if (menu) g_press_ns = g_menu_ns.load(), g_window_open = false;
    update(); assert(g_s.menu == menu);
}
static ImVec2 hud(int zone, float x, float y) {
    touch_hud_piece(zone, x, y, x, y, 0, 0); touch_hud_frame();
    Rect r = zone_rect(zones(), zone, (float)screen_w, (float)screen_h); assert(r.ok);
    return ImVec2((r.x0 + r.x1) * .5f, (r.y0 + r.y1) * .5f);
}
static int pulses(uint64_t buttons) {
    int n = 0; for (const Pulse &p : g_s.pulse) if (p.until > SDL_GetTicksNS() && p.buttons == buttons) ++n;
    if (g_s.ammo_pulse.until > SDL_GetTicksNS() && g_s.ammo_pulse.buttons == buttons) ++n;
    return n;
}
static void touch_button_sources() {
    fresh();
    Input physical = update(B(BTN_A));
    assert(physical.b == B(BTN_A) && physical.touch_b == 0 && g_press_ns != 0);
    at(SDL_EVENT_FINGER_DOWN, C_HEAVY);
    Input overlap = update(B(BTN_A));
    assert(overlap.b == B(BTN_A) && overlap.touch_b == B(BTN_A));
    at(SDL_EVENT_FINGER_CANCELED, C_HEAVY);
    assert(update(B(BTN_A)).touch_b == 0);
    at(SDL_EVENT_FINGER_DOWN, C_HEAVY); at(SDL_EVENT_FINGER_UP, C_HEAVY);
    Input tap = update(B(BTN_X));
    assert(tap.b == (B(BTN_A) | B(BTN_X)) && tap.touch_b == B(BTN_A));
    settings_open = true;
    Input menu = update(B(BTN_X));
    assert(menu.b == B(BTN_X) && menu.touch_b == 0);
    settings_open = false; touch_reset(); cfg.touch_controls = TOUCH_OFF;
    Input hidden = update(B(BTN_A));
    assert(hidden.b == B(BTN_A) && hidden.touch_b == 0);
    puts("PASS: touch source word includes held and tapped buttons even when physical buttons overlap; physical menu detection and overlay suppression stay intact");
}
static void item_gestures() {
    fresh(); ImVec2 p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 1, p.x, p.y); assert(finger(g_s, 1)->kind == F_ITEM);
    event(SDL_EVENT_FINGER_MOTION, 1, p.x + 240, p.y); assert(item_steps == std::vector<int>{1});
    assert(update().touch_b == B(BTN_L) && !finger(g_s, 1)->done);
    finger(g_s, 1)->step_at = 0;
    event(SDL_EVENT_FINGER_MOTION, 1, p.x + 400, p.y);
    assert(item_steps == (std::vector<int>{1,1}));
    finger(g_s, 1)->step_at = 0;
    event(SDL_EVENT_FINGER_MOTION, 1, p.x - 300, p.y);
    assert(item_steps == (std::vector<int>{1,1,-1}));
    finger(g_s, 1)->step_at = 0;
    event(SDL_EVENT_FINGER_UP, 1, p.x - 500, p.y);
    assert(item_steps == (std::vector<int>{1,1,-1,-1}));
    assert(!update().b && touch_item_pending==-1); // normal up leaves one short-lived native request
    event(SDL_EVENT_FINGER_DOWN, 2, p.x, p.y);
    assert(!touch_item_pending); // a new selection starts with no previous gesture's pending step
    event(SDL_EVENT_FINGER_UP, 2, p.x - 200, p.y);
    assert(item_steps == (std::vector<int>{1,1,-1,-1,-1}) && !update().b);
    event(SDL_EVENT_FINGER_DOWN, 3, p.x, p.y);
    event(SDL_EVENT_FINGER_MOTION, 3, p.x + 25, p.y);
    event(SDL_EVENT_FINGER_UP, 3, p.x + 30, p.y); assert(item_steps.size() == 5); // no lower release threshold
    event(SDL_EVENT_FINGER_DOWN, 4, p.x, p.y); event(SDL_EVENT_FINGER_UP, 4, p.x, p.y);
    assert(update().b == B(BTN_Y));
    fresh(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 1, p.x, p.y);
    finger(g_s, 1)->t0 = SDL_GetTicksNS() - 250 * MS;
    assert(update().b == B(BTN_L) && update().touch_b == B(BTN_L));
    event(SDL_EVENT_FINGER_UP, 1, p.x, p.y); assert(!update().b && !pulses(B(BTN_Y)));
    event(SDL_EVENT_FINGER_DOWN, 2, p.x, p.y); event(SDL_EVENT_FINGER_UP, 2, p.x, p.y);
    assert(update().b == B(BTN_Y));
    fresh(); ranged = WEAPON_BOWGUN; update(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 5, p.x, p.y);
    event(SDL_EVENT_FINGER_MOTION, 5, p.x, p.y - 180); assert(pulses(B(BTN_X)) == 1);
    const uint64_t ready = g_s.ammo_ready;
    event(SDL_EVENT_FINGER_MOTION, 5, p.x, p.y + 200);
    assert(pulses(B(BTN_X)) == 0 && pulses(B(BTN_B)) == 1 && g_s.ammo_ready == ready);
    g_s.ammo_pulse = {}; g_s.ammo_ready = 0;
    event(SDL_EVENT_FINGER_MOTION, 5, p.x, p.y - 180);
    assert(pulses(B(BTN_X)) == 1 && pulses(B(BTN_B)) == 0 && item_steps.empty());
    g_s.ammo_pulse.from = 0; g_s.ammo_pulse.until = SDL_GetTicksNS() + 80 * MS;
    Input ammo = update(B(BTN_L) | B(BTN_X));
    assert(ammo.b == (B(BTN_L) | B(BTN_X)) && ammo.touch_b == ammo.b);
    event(SDL_EVENT_FINGER_UP, 5, p.x, p.y - 180); assert(!pulses(B(BTN_Y)));
    fresh(); ranged = WEAPON_BOWGUN; update(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 6, p.x, p.y); event(SDL_EVENT_FINGER_UP, 6, p.x, p.y + 150);
    assert(pulses(B(BTN_B)) == 1 && item_steps.empty() && update().b == B(BTN_L));
    puts("PASS: continuous item and ammo slides reverse under the same finger, lift and reenter, long hold opens L, short tap uses Y, and native touch sources include L/X/B");
}
static void auto_r() {
    fresh(); at(SDL_EVENT_FINGER_DOWN, C_STICK);
    at(SDL_EVENT_FINGER_MOTION, C_STICK, 1, 2, 0);
    Input on = update(); assert(on.lx > 30000 && (on.b & B(BTN_R)) && g_s.run);
    cfg.touch_auto_r = 0; Input off = update();
    assert(off.lx == on.lx && off.ly == on.ly && !(off.b & B(BTN_R)) && !g_s.run);
    at(SDL_EVENT_FINGER_DOWN, C_R, 2); assert(update().b & B(BTN_R));
    at(SDL_EVENT_FINGER_CANCELED, C_R, 2); assert(!(update().b & B(BTN_R)));
    assert(update(B(BTN_R), true).b & B(BTN_R));
    cfg.touch_auto_r = 1; assert(update().b & B(BTN_R));
    drawn = 1; assert(!(update().b & B(BTN_R)) && update().lx > 30000);
    at(SDL_EVENT_FINGER_CANCELED, C_STICK); assert(update().lx == 0);
    puts("PASS: automatic R default/on/off preserves movement, touch R, controller R, drawn weapon");
}
static void floating_stick() {
    fresh(); Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
    float x=g.c.x+g.r*.45f,y=g.c.y-g.r*.2f;
    event(SDL_EVENT_FINGER_DOWN,1,x,y);assert(finger(g_s,1)->kind==F_STICK);
    assert(fabsf(g_s.bx-g.c.x)<.01f&&fabsf(g_s.by-g.c.y)<.01f&&update().lx>0);
    event(SDL_EVENT_FINGER_CANCELED,1,x,y);
    cfg.touch_floating=1;event(SDL_EVENT_FINGER_DOWN,2,x,y);
    assert(finger(g_s,2)->kind==F_STICK&&fabsf(g_s.bx-x)<.01f&&fabsf(g_s.by-y)<.01f);
    assert(update().lx==0&&update().ly==0);
    event(SDL_EVENT_FINGER_MOTION,2,x+g.r*2,y);assert(update().lx>30000&&(update().b&B(BTN_R)));
    cfg.touch_auto_r=0;assert(update().lx>30000&&!(update().b&B(BTN_R)));
    event(SDL_EVENT_FINGER_CANCELED,2,x+g.r*2,y);
    x=screen_w*.42f;y=screen_h*.55f;cfg.touch_floating=0;
    event(SDL_EVENT_FINGER_DOWN,3,x,y);assert(finger(g_s,3)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_CANCELED,3,x,y);cfg.touch_floating=1;
    event(SDL_EVENT_FINGER_DOWN,4,x,y);assert(finger(g_s,4)->kind==F_STICK&&update().lx==0);
    event(SDL_EVENT_FINGER_CANCELED,4,x,y);
    ImVec2 p=hud(TZ_ITEM,x,y);event(SDL_EVENT_FINGER_DOWN,5,p.x,p.y);assert(finger(g_s,5)->kind==F_ITEM);
    event(SDL_EVENT_FINGER_CANCELED,5,p.x,p.y);g_zones={};
    game_menu_state(true);event(SDL_EVENT_FINGER_DOWN,6,x,y);
    event(SDL_EVENT_FINGER_MOTION,6,x,y-g.r);assert(update().ly>30000 && !(update().b&DPAD_UP));
    event(SDL_EVENT_FINGER_CANCELED,6,x,y-g.r);assert(update().ly==0);
    puts("PASS: fixed/floating stick origin, neutral landing, broader floating area, item priority, menu movement and AutoR");
}
static std::vector<ImVec2> touch_geometry() {
    ImGuiIO &io = ImGui::GetIO(); io.DisplaySize = ImVec2((float)screen_w, (float)screen_h); io.DeltaTime = 1.0f/60;
    ImGui::NewFrame(); touch_draw((float)screen_w, (float)screen_h);
    std::vector<ImVec2> vertices; ImDrawList *list = g_s.edit ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
    for (const ImDrawVert &v:list->VtxBuffer) vertices.push_back(v.pos);
    ImGui::Render(); return vertices;
}
static int drawn_vertices(ImVec2 centre, float r) {
    int n=0;
    for (const ImVec2 &p:touch_geometry()) if (fabsf(p.x-centre.x)<r && fabsf(p.y-centre.y)<r) ++n;
    return n;
}
static void same_geometry(const std::vector<ImVec2> &a,const std::vector<ImVec2> &b) {
    assert(a.size()==b.size());
    for (size_t i=0;i<a.size();++i) assert(a[i].x==b[i].x && a[i].y==b[i].y);
}
static void dpad_visibility(int w, int h) {
    fresh(w,h); cfg.touch_show[TB_DPAD]=0; Geo toggle = place(g_s.layout, C_DPAD_TOGGLE, (float)w, (float)h);
    assert(!cfg.touch_show[TB_DPAD] && drawn_vertices(toggle.c,toggle.r) > 20);
    at(SDL_EVENT_FINGER_DOWN, C_DPAD_TOGGLE, 1); assert(cfg.touch_show[TB_DPAD]);
    at(SDL_EVENT_FINGER_UP, C_DPAD_TOGGLE, 1);
    at(SDL_EVENT_FINGER_DOWN, C_HDPAD, 2, 0, -.65f); assert(update().b & DPAD_UP);
    at(SDL_EVENT_FINGER_DOWN, C_DPAD_TOGGLE, 3); assert(!cfg.touch_show[TB_DPAD]);
    assert(update(DPAD_LEFT,true).b == DPAD_LEFT && !g_s.held);
    at(SDL_EVENT_FINGER_MOTION, C_HDPAD, 2, .65f, 0); assert(update().b == 0);
    at(SDL_EVENT_FINGER_UP, C_HDPAD, 2); at(SDL_EVENT_FINGER_UP, C_DPAD_TOGGLE, 3);
    assert(drawn_vertices(toggle.c,toggle.r) > 20);
    at(SDL_EVENT_FINGER_DOWN, C_DPAD_TOGGLE, 4); at(SDL_EVENT_FINGER_UP, C_DPAD_TOGGLE, 4);
    at(SDL_EVENT_FINGER_DOWN, C_HDPAD, 5, 0, -.65f); at(SDL_EVENT_FINGER_UP, C_HDPAD, 5, 0, -.65f);
    assert(pulses(DPAD_UP) == 1); at(SDL_EVENT_FINGER_DOWN, C_DPAD_TOGGLE, 6);
    assert(pulses(DPAD_UP) == 0 && update(DPAD_RIGHT,true).b == DPAD_RIGHT);
    at(SDL_EVENT_FINGER_UP, C_DPAD_TOGGLE, 6);
    // Hunter Arts use the same direction bits, but their pulses must remain when the D-pad is hidden.
    ImVec2 art = hud(TZ_ARTS, 640, 280);
    event(SDL_EVENT_FINGER_DOWN, 7, art.x, art.y); assert(finger(g_s,7)->kind == F_ARTS);
    event(SDL_EVENT_FINGER_UP, 7, art.x, art.y); assert(update().b & DPAD_UP);
    touch_reset(); game_menu_state(true); assert(!in_mode(C_HDPAD,true,true));
    at(SDL_EVENT_FINGER_DOWN, C_DPAD_TOGGLE, 8); at(SDL_EVENT_FINGER_UP, C_DPAD_TOGGLE, 8);
    assert(in_mode(C_HDPAD,true,true)); at(SDL_EVENT_FINGER_DOWN,C_HDPAD,9,0,-.65f); assert(update().b & DPAD_UP);
    at(SDL_EVENT_FINGER_CANCELED,C_HDPAD,9,0,-.65f); game_menu_state(false);
    assert(in_mode(C_HDPAD,false,true));
    cfg.touch_controls=TOUCH_OFF; assert(update(DPAD_DOWN,true).b == DPAD_DOWN);
    assert(drawn_vertices(toggle.c,toggle.r) == 0);
    Geo d = place(g_s.layout,C_HDPAD,(float)w,(float)h); assert(drawn_vertices(d.c,d.r*.75f) == 0);
    at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE,10); assert(cfg.touch_show[TB_DPAD]);
    at(SDL_EVENT_FINGER_CANCELED,C_DPAD_TOGGLE,10);
    at(SDL_EVENT_FINGER_DOWN,C_SETTINGS,11);update();at(SDL_EVENT_FINGER_UP,C_SETTINGS,11);
    assert(settings_open);settings_open=false;
    printf("PASS: D-pad %dx%d visibility, shared menu/hunt state, held and tap release, art pulse and controller preserved, global Off\n",w,h);
}
static void editor_click(int button) {
    Rect panel, btn[E_COUNT]; editor_panel((float)screen_w,(float)screen_h,&panel,btn);
    float x=(btn[button].x0+btn[button].x1)*.5f,y=(btn[button].y0+btn[button].y1)*.5f;
    event(SDL_EVENT_FINGER_DOWN,9,x,y); event(SDL_EVENT_FINGER_UP,9,x,y);
}
static void editor_page(int wanted) {
    assert(g_s.edit && !g_s.in_quest);
    for (int step=0;g_s.layout_profile!=wanted && step<LAYOUT_SLOTS;++step) {
        editor_click(E_SET); update();
    }
    assert(g_s.layout_profile==wanted);
}
static void editor_bind(int wanted) {
    assert(g_s.edit && !g_s.in_quest && reserved_layout(g_s.layout_profile));
    for (int step=0;control_profile(g_s)!=wanted && step<TOUCH_WEAPON_LAYOUTS;++step) {
        editor_click(E_STYLE); update();
    }
    assert(control_profile(g_s)==wanted);
}
static void same_layout(const Layout &a, const Layout &b) {
    assert(a.style==b.style && a.weapon_profile==b.weapon_profile);
    for (int c=0;c<C_COUNT;++c) {
        assert(fabsf(a.x[c]-b.x[c])<.0001f && fabsf(a.y[c]-b.y[c])<.0001f);
        assert(fabsf(a.scale[c]-b.scale[c])<.001f);
    }
    for (int z=0;z<TZ_COUNT;++z) assert(fabsf(a.zx[z]-b.zx[z])<.11f && fabsf(a.zy[z]-b.zy[z])<.11f);
}
static std::string stored_layout_text(int p) {
    char path[40]; layout_path(p,path,sizeof path); std::ifstream f(path); assert(f);
    return std::string(std::istreambuf_iterator<char>(f),{});
}
static void layout_roundtrip() {
    fresh(); touch_edit_begin(); update(); assert(g_s.edit);
    Geo g=place(g_s.layout,C_DPAD_TOGGLE,(float)screen_w,(float)screen_h);
    event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y); assert(g_s.sel==C_DPAD_TOGGLE);
    event(SDL_EVENT_FINGER_MOTION,1,g.c.x+110,g.c.y+45); event(SDL_EVENT_FINGER_UP,1,g.c.x+110,g.c.y+45);
    assert(fabsf(g_s.layout.x[C_DPAD_TOGGLE]-DEFS[C_DPAD_TOGGLE].x)>0.1f);
    editor_click(E_BIGGER); assert(g_s.layout.scale[C_DPAD_TOGGLE]>1.09f);
    Geo moved=place(g_s.layout,C_DPAD_TOGGLE,(float)screen_w,(float)screen_h);
    event(SDL_EVENT_FINGER_DOWN,1,moved.c.x,moved.c.y);
    event(SDL_EVENT_FINGER_DOWN,2,moved.c.x+100,moved.c.y); assert(g_s.pinch);
    event(SDL_EVENT_FINGER_MOTION,2,moved.c.x+150,moved.c.y); assert(g_s.layout.scale[C_DPAD_TOGGLE]>1.6f);
    event(SDL_EVENT_FINGER_UP,2,moved.c.x+150,moved.c.y); event(SDL_EVENT_FINGER_UP,1,moved.c.x,moved.c.y);
    const Layout before=g_s.layout; editor_click(E_SAVE); assert(!g_s.edit);
    char path[40]; layout_path(g_s.layout_profile,path,sizeof path);
    std::ifstream f(path); const std::string text{std::istreambuf_iterator<char>(f),{}};
    assert(text.find("dpad_toggle ")!=std::string::npos);
    Layout loaded; assert(load_layout(&loaded,g_s.layout_profile));
    assert(fabsf(loaded.x[C_DPAD_TOGGLE]-before.x[C_DPAD_TOGGLE])<.0001f);
    assert(fabsf(loaded.y[C_DPAD_TOGGLE]-before.y[C_DPAD_TOGGLE])<.0001f);
    assert(fabsf(loaded.scale[C_DPAD_TOGGLE]-before.scale[C_DPAD_TOGGLE])<.001f);
    puts("PASS: shortcut editor drag, +/- size, pinch size and layout file round trip");
}
static void profile_layouts() {
    fresh();
    for (int p=0;p<TOUCH_LAYOUT_PROFILES;++p) {
        Layout l=defaults(p);
        l.x[C_DPAD_TOGGLE]=.2f+p*.015f; l.y[C_DPAD_TOGGLE]=.76f-p*.008f; l.scale[C_DPAD_TOGGLE]=.6f+p*.07f;
        save_layout(l,p);
    }
    const std::string old_pill=stored_layout_text(TOUCH_WEAPON_LAYOUTS),old_corner=stored_layout_text(TOUCH_WEAPON_LAYOUTS+2);
    cfg.touch_show[TB_DPAD]=0;
    for (int p=1;p<TOUCH_LAYOUT_PROFILES;++p) {
        select_layout(g_s,p);
        const int selected=p==TOUCH_WEAPON_LAYOUTS || p==TOUCH_WEAPON_LAYOUTS+2?TOUCH_WEAPON_LAYOUTS+1:p;
        assert(profile==selected && !cfg.touch_show[TB_DPAD]);
        assert(fabsf(g_s.layout.x[C_DPAD_TOGGLE]-(.2f+selected*.015f))<.0001f);
        assert(fabsf(g_s.layout.y[C_DPAD_TOGGLE]-(.76f-selected*.008f))<.0001f);
        assert(fabsf(g_s.layout.scale[C_DPAD_TOGGLE]-(.6f+selected*.07f))<.001f);
        Geo hunt=place(g_s.layout,C_HDPAD,(float)screen_w,(float)screen_h);
        Geo menu=place(g_s.layout,C_DPAD,(float)screen_w,(float)screen_h);
        assert(hunt.c.x==menu.c.x && hunt.c.y==menu.c.y && hunt.r==menu.r);
    }
    assert(stored_layout_text(TOUCH_WEAPON_LAYOUTS)==old_pill && stored_layout_text(TOUCH_WEAPON_LAYOUTS+2)==old_corner);
    const int legacy=TOUCH_LAYOUT_PROFILES-1; char path[40];layout_path(legacy,path,sizeof path);
    { std::ofstream f(path);f<<"attack 0.3 0.4 1.2\n"; }
    Layout loaded;assert(load_layout(&loaded,legacy));
    assert(fabsf(loaded.x[C_ATTACK]-.3f)<.0001f && fabsf(loaded.scale[C_ATTACK]-1.2f)<.001f);
    assert(loaded.x[C_DPAD_TOGGLE]==DEFS[C_DPAD_TOGGLE].x && loaded.scale[C_DPAD_TOGGLE]==1);
    in_quest=false;update();assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+g_s.legacy_mode);
    in_quest=true;ranged=WEAPON_BOW;update();assert(g_s.layout_profile==profile_for_weapon(10));
    puts("PASS: 15 weapon and 4 compatible legacy files retain coordinates; removed pill/corner selection falls back to buttons without changing old files, shared visibility and automatic profile switching survive");
}
static void legacy_startup_compatibility() {
    assert(TOUCH_LEGACY_LAYOUTS==4 && LAYOUT_SLOTS==24 && State{}.legacy_mode==1);
    for (int old_mode:{0,2}) {
        fresh();
        Layout old=defaults(TOUCH_WEAPON_LAYOUTS+old_mode); old.x[C_DPAD_TOGGLE]=.481f+old_mode*.01f;
        old.x[C_RSTICK]=.381f+old_mode*.01f; old.scale[C_RSTICK]=1.23f;
        assert(save_layout(old,TOUCH_WEAPON_LAYOUTS+old_mode));
        const std::string original=stored_layout_text(TOUCH_WEAPON_LAYOUTS+old_mode);
        Layout buttons=defaults(TOUCH_WEAPON_LAYOUTS+1); buttons.x[C_DPAD_TOGGLE]=.371f;
        assert(save_layout(buttons,TOUCH_WEAPON_LAYOUTS+1));
        profile=TOUCH_WEAPON_LAYOUTS+old_mode; in_quest=false; g_s.loaded=false; update();
        assert(g_s.legacy_mode==1 && g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+1 && profile==TOUCH_WEAPON_LAYOUTS+1);
        assert(g_style==STYLE_SEPARATE && fabsf(g_s.layout.x[C_DPAD_TOGGLE]-.371f)<.0001f);
        assert(stored_layout_text(TOUCH_WEAPON_LAYOUTS+old_mode)==original);
        for (int requested:{0,2,-1,4}) {
            select_legacy_mode(g_s,3); select_legacy_mode(g_s,requested);
            assert(g_s.legacy_mode==1 && g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+1 && profile==TOUCH_WEAPON_LAYOUTS+1);
            assert(stored_layout_text(TOUCH_WEAPON_LAYOUTS+old_mode)==original);
        }
    }
    puts("PASS: old legacy modes 0/2 boot into buttons, other removed/invalid mode requests normalize to buttons, and every old layout byte including hidden right-stick coordinates remains intact");
}
static void nonquest_layouts() {
    fresh(); in_quest=false; update();
    for (int menu=0;menu<2;++menu) {
        game_menu_state(menu!=0);
        select_legacy_mode(g_s,1);
        for (int step=0;step<8;++step) {
            const int mode=step%2?3:1;
            assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+mode);
            assert(g_style==legacy_style(mode));
            assert(in_mode(C_LAYOUT,g_s.menu,false));
            assert(!in_mode(C_ATTACK,g_s.menu,false));
            assert(in_mode(C_HEAVY,g_s.menu,false)==(mode==1));
            assert(!in_mode(C_CORNER,g_s.menu,false));
            assert(in_mode(C_RFIRE,g_s.menu,false)==(mode==3));
            assert(!in_mode(C_MPILL,g_s.menu,false));
            at(SDL_EVENT_FINGER_DOWN,C_LAYOUT); at(SDL_EVENT_FINGER_UP,C_LAYOUT); update();
        }
        assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+1 && g_s.legacy_mode==1);
    }
    in_quest=true; update(); game_menu_state(true);
    assert(!in_mode(C_MPILL,true,true) && !in_mode(C_LAYOUT,true,true));
    assert(in_mode(C_HEAVY,true,true) && in_mode(C_DODGE,true,true));
    puts("PASS: villages and game menus repeatedly alternate only attack-buttons/ranged layouts; quests keep attack buttons and cannot cycle layouts");
}
static void nonquest_editor_profiles() {
    std::vector<int> pages;
    for (int p=0;p<LAYOUT_SLOTS;++p) {
        const bool allowed=p!=TOUCH_WEAPON_LAYOUTS && p!=TOUCH_WEAPON_LAYOUTS+2;
        assert(available_layout(p)==allowed);
        if (allowed) pages.push_back(p);
    }
    assert(pages.size()==22);
    for (int mode:{1,3}) {
        fresh();
        for (int p=0;p<LAYOUT_SLOTS;++p) {
            Layout l=defaults(p);
            if (ranged_profile(p)) l.style=STYLE_CORNER;
            save_layout(l,p);
        }
        in_quest=false; update(); select_legacy_mode(g_s,mode);
        const int entry=TOUCH_WEAPON_LAYOUTS+mode;
        const size_t start=std::find(pages.begin(),pages.end(),entry)-pages.begin();
        bool seen[LAYOUT_SLOTS]={}; int weapon_pages=0,legacy_pages=0;
        touch_edit_begin(); update();
        for (size_t step=0;step<pages.size();++step) {
            const int p=pages[(start+step)%pages.size()];
            assert(!seen[p]); seen[p]=true;
            if (p<TOUCH_WEAPON_LAYOUTS || p>=UNIVERSAL_LAYOUT) ++weapon_pages; else ++legacy_pages;
            const int style=p==UNIVERSAL_LAYOUT?STYLE_CLASSIC:reserved_layout(p)?STYLE_PILL:p>=TOUCH_WEAPON_LAYOUTS?legacy_style(p-TOUCH_WEAPON_LAYOUTS):ranged_profile(p)?STYLE_CORNER:STYLE_SEPARATE;
            for (int frame=0;frame<3;++frame) {
                update();
                assert(g_s.edit && g_s.layout_profile==p && profile==entry && g_s.legacy_mode==mode);
                assert(g_s.layout.style==style && g_style==style && g_ranged==ranged_layout_profile(p));
                if (p==UNIVERSAL_LAYOUT) {
                    assert(control_profile(g_s)==UNIVERSAL_LAYOUT && g_ranged==0);
                    assert(in_editor(C_HEAVY,false,UNIVERSAL_LAYOUT) && in_editor(C_LIGHT,false,UNIVERSAL_LAYOUT));
                    assert(in_editor(C_STICK,false,UNIVERSAL_LAYOUT) && !in_editor(C_RSTICK,false,UNIVERSAL_LAYOUT));
                    assert(!in_editor(C_RFIRE,false,UNIVERSAL_LAYOUT) && !in_editor(C_COMBO,false,UNIVERSAL_LAYOUT));
                }
                if (reserved_layout(p)) {
                    assert(control_profile(g_s)==-1);
                    for (int c=0;c<C_COUNT;++c) assert(!in_editor(c,false,control_profile(g_s)));
                }
            }
            editor_click(E_SET); update();
        }
        assert(g_s.layout_profile==entry && weapon_pages==20 && legacy_pages==2);
        for (int p=0;p<LAYOUT_SLOTS;++p) assert(seen[p]==(p!=TOUCH_WEAPON_LAYOUTS && p!=TOUCH_WEAPON_LAYOUTS+2));
        const int savedp=profile_for_weapon(mode==1?15:10);
        editor_page(savedp);
        g_s.layout.x[C_DPAD_TOGGLE]=.345f; g_s.layout.y[C_DPAD_TOGGLE]=.678f; g_s.layout.scale[C_DPAD_TOGGLE]=1.25f;
        editor_click(E_SAVE); update();
        assert(!g_s.edit && g_s.layout_profile==entry && profile==entry && g_s.legacy_mode==mode);
        Layout saved; assert(load_layout(&saved,savedp));
        assert(fabsf(saved.x[C_DPAD_TOGGLE]-.345f)<.0001f && fabsf(saved.y[C_DPAD_TOGGLE]-.678f)<.0001f && fabsf(saved.scale[C_DPAD_TOGGLE]-1.25f)<.001f);
        in_quest=true; weapon_override=PROFILE_WEAPON[savedp]; drawn=1; update();
        assert(g_s.layout_profile==savedp && profile==savedp && g_s.legacy_mode==mode);
        assert(fabsf(g_s.layout.x[C_DPAD_TOGGLE]-.345f)<.0001f && fabsf(g_s.layout.scale[C_DPAD_TOGGLE]-1.25f)<.001f);
        in_quest=false; update(); assert(g_s.layout_profile==entry && profile==entry);
        for (int step=1;step<=4;++step) {
            at(SDL_EVENT_FINGER_DOWN,C_LAYOUT); at(SDL_EVENT_FINGER_UP,C_LAYOUT); update();
            const int expected=step%2?4-mode:mode;
            assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+expected && g_s.legacy_mode==expected && profile==g_s.layout_profile);
        }
        touch_edit_begin(); update(); editor_page(0);
        g_s.layout.x[C_DPAD_TOGGLE]=.281f;
        editor_click(E_SET); update(); assert(g_s.layout_profile==1);
        const float original=g_s.layout.x[C_DPAD_TOGGLE]; g_s.layout.x[C_DPAD_TOGGLE]=.9f;
        editor_click(E_CANCEL); update();
        assert(!g_s.edit && g_s.layout_profile==entry && profile==entry && g_s.legacy_mode==mode);
        assert(load_layout(&saved,0) && fabsf(saved.x[C_DPAD_TOGGLE]-.281f)<.0001f);
        assert(load_layout(&saved,1) && fabsf(saved.x[C_DPAD_TOGGLE]-original)<.0001f);
    }
    puts("PASS: both generic entries traverse exactly 22 editor pages: 20 weapon presets and 2 legacy layouts; removed slots are skipped, Save/Cancel return, page autosave and live bindings survive");
}
static void editor_combo_isolation() {
    fresh(); in_quest=false; update(); select_legacy_mode(g_s,1);
    const int entry=TOUCH_WEAPON_LAYOUTS+1,bow=profile_for_weapon(10);
    strcpy(cfg.touch_weapon_combo[entry][0],"A"); strcpy(cfg.touch_weapon_combo[bow][1],"X+A");
    touch_edit_begin(); update(); editor_page(bow);
    assert(profile==entry && g_s.legacy_mode==1);
    assert(!in_editor(C_COMBO,false,bow) && in_editor(C_COMBO+1,false,bow));
    assert(in_mode(C_COMBO,false,false) && !in_mode(C_COMBO+1,false,false));
    char label[32]; combo_label(C_COMBO+1,bow,label,sizeof label); assert(!strcmp(label,"X+A"));
    assert(config_live_touch_combo(config_live_touch_profile(),0)==B(BTN_A));
    Finger f{}; f.ctl=C_COMBO; assert(finger_buttons(f)==B(BTN_A));
    f.ctl=C_COMBO+1; assert(finger_buttons(f)==0);
    editor_click(E_CANCEL); update();
    assert(!g_s.edit && profile==entry && g_s.layout_profile==entry && g_s.legacy_mode==1);
    puts("PASS: editor combo visibility/labels follow the preview while live combo dispatch and return profile stay unchanged");
}
static void editor_scene_transitions() {
    fresh(); in_quest=false; update(); select_legacy_mode(g_s,3);
    const int entry=TOUCH_WEAPON_LAYOUTS+3,bow=profile_for_weapon(10),cat=profile_for_weapon(15);
    touch_edit_begin(); update(); editor_page(bow); g_s.layout.x[C_DPAD_TOGGLE]=.342f;
    in_quest=true; weapon_override=15; update();
    assert(!g_s.edit && g_s.layout_profile==cat && profile==cat && g_s.legacy_mode==3);
    Layout saved; assert(load_layout(&saved,bow) && fabsf(saved.x[C_DPAD_TOGGLE]-.342f)<.0001f);
    touch_edit_begin(); update(); assert(g_s.edit); g_s.layout.x[C_DPAD_TOGGLE]=.456f;
    weapon_override=10; update();
    assert(!g_s.edit && g_s.layout_profile==bow && profile==bow && g_s.legacy_mode==3);
    assert(load_layout(&saved,cat) && fabsf(saved.x[C_DPAD_TOGGLE]-.456f)<.0001f);
    touch_edit_begin(); update(); assert(g_s.edit); g_s.layout.x[C_DPAD_TOGGLE]=.567f;
    in_quest=false; update();
    assert(!g_s.edit && g_s.layout_profile==entry && profile==entry && g_s.legacy_mode==3);
    assert(load_layout(&saved,bow) && fabsf(saved.x[C_DPAD_TOGGLE]-.567f)<.0001f);
    puts("PASS: entering/leaving quests and changing local weapon save and end editing before selecting the real profile");
}
static void preset_fixture(int mode=1) {
    fresh();
    assert(TOUCH_WEAPON_LAYOUTS+1+RESERVED_LAYOUT_COUNT==20 && RESERVED_LAYOUT_COUNT==4 && LAYOUT_SLOTS==24);
    for (int p=0;p<LAYOUT_SLOTS;++p) assert(save_layout(defaults(p),p));
    in_quest=false; update(); select_legacy_mode(g_s,mode);
}
static void reserved_preset_slots() {
    preset_fixture(); const int lbg=profile_for_weapon(6),entry=TOUCH_WEAPON_LAYOUTS+1;
    Layout base=defaults(lbg); base.style=STYLE_CORNER;
    base.x[C_DPAD_TOGGLE]=.451f; base.y[C_DPAD_TOGGLE]=.732f; base.scale[C_DPAD_TOGGLE]=1.31f; base.zx[TZ_ITEM]=12.5f;
    assert(save_layout(base,lbg));
    touch_edit_begin(); update();
    for (int i=0;i<RESERVED_LAYOUT_COUNT;++i) {
        const int slot=RESERVED_LAYOUT_BASE+i; editor_page(slot);
        assert(control_profile(g_s)==-1); editor_click(E_SAVE); update(); assert(g_s.edit && profile==entry);
        Layout inherited; assert(load_layout(&inherited,active_layout(g_s,lbg)));
        editor_bind(lbg); same_layout(g_s.layout,inherited);
        g_s.layout.x[C_DPAD_TOGGLE]=.6f+i*.02f; g_s.layout.scale[C_DPAD_TOGGLE]=1.1f+i*.05f;
        editor_click(E_SAVE); update(); assert(!g_s.edit && profile==entry);
        touch_edit_begin(); update(); editor_page(slot); g_s.layout.y[C_DPAD_TOGGLE]=.701f+i*.01f;
        editor_click(E_SET); update();
        assert(profile==entry && g_s.legacy_mode==1);
        for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==lbg?slot:p));
    }
    Layout saved;
    for (int i=0;i<RESERVED_LAYOUT_COUNT;++i) {
        assert(load_layout(&saved,RESERVED_LAYOUT_BASE+i) && saved.weapon_profile==lbg);
        assert(fabsf(saved.x[C_DPAD_TOGGLE]-(.6f+i*.02f))<.0001f && fabsf(saved.scale[C_DPAD_TOGGLE]-(1.1f+i*.05f))<.001f);
        assert(fabsf(saved.y[C_DPAD_TOGGLE]-(.701f+i*.01f))<.0001f);
    }
    assert(load_layout(&saved,lbg)); same_layout(saved,base);
    editor_click(E_CANCEL); update(); assert(!g_s.edit && profile==entry);
    puts("PASS: four reserved slots inherit the bound weapon's active layout and retain independent coordinates/size; Save confirms binding, same-binding page autosave preserves the selection, unbound Save is disabled");
}
static void reserved_activation_rebinding() {
    preset_fixture(); const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15),first=RESERVED_LAYOUT_BASE,second=first+1;
    const int entry=TOUCH_WEAPON_LAYOUTS+1;
    touch_edit_begin(); update(); editor_page(first); editor_bind(lbg); g_s.layout.x[C_DPAD_TOGGLE]=.613f;
    editor_click(E_SAVE); update(); assert(!g_s.edit && profile==entry && g_s.layout_profile==entry);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==lbg?first:p));
    touch_edit_begin(); update(); editor_page(second); editor_bind(lbg);
    assert(fabsf(g_s.layout.x[C_DPAD_TOGGLE]-.613f)<.0001f);  // Inherit the selected preset, not only the fixed base.
    g_s.layout.x[C_DPAD_TOGGLE]=.723f; editor_click(E_SAVE); update();
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==lbg?second:p));
    Layout saved; assert(load_layout(&saved,first) && fabsf(saved.x[C_DPAD_TOGGLE]-.613f)<.0001f);
    touch_edit_begin(); update(); editor_page(second); editor_bind(cat); g_s.layout.x[C_DPAD_TOGGLE]=.834f;
    editor_click(E_SAVE); update();
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==cat?second:p));
    assert(active_layout(g_s,lbg)==lbg && active_layout(g_s,cat)==second && profile==entry);
    State restored{}; load_preset_selection(restored);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(active_layout(restored,p)==(p==cat?second:p));
    touch_edit_begin(); update(); editor_page(cat); const Layout fixed=g_s.layout;
    editor_click(E_STYLE); same_layout(g_s.layout,fixed); editor_click(E_SAVE); update();
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==p);
    puts("PASS: Save selects only the bound weapon, binding inherits its active preset, rebinding clears the old weapon's reference, and saving a fixed slot restores its own base");
}
static void reserved_cancel_defaults() {
    preset_fixture(); const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15),slot=RESERVED_LAYOUT_BASE;
    Layout original=defaults(lbg); original.x[C_DPAD_TOGGLE]=.654f; assert(save_layout(original,slot)); g_s.weapon_layout[lbg]=slot;
    touch_edit_begin(); update(); editor_page(slot); editor_bind(cat); g_s.layout.x[C_DPAD_TOGGLE]=.912f;
    editor_click(E_CANCEL); update();
    Layout saved; assert(load_layout(&saved,slot)); same_layout(saved,original);
    assert(active_layout(g_s,lbg)==slot && active_layout(g_s,cat)==cat);
    touch_edit_begin(); update(); editor_page(slot); editor_click(E_RESET);
    same_layout(g_s.layout,defaults(lbg)); assert(control_profile(g_s)==lbg);
    editor_click(E_SAVE); update(); assert(active_layout(g_s,lbg)==slot);
    touch_edit_begin(); update(); editor_page(slot+1); editor_click(E_RESET);
    assert(control_profile(g_s)==-1); editor_click(E_SAVE); update(); assert(g_s.edit);
    editor_click(E_CANCEL); update(); assert(!g_s.edit);
    puts("PASS: Cancel restores the current page's binding and coordinates without activating it; Defaults keeps an existing binding and leaves an unbound slot unassigned");
}
static void preset_mapping_reload() {
    preset_fixture(); const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15),first=RESERVED_LAYOUT_BASE,second=first+1;
    assert(save_layout(defaults(lbg),first) && save_layout(defaults(cat),second) && save_layout(defaults(0),first+2));
    {
        std::ofstream f("touch_weapon_presets.txt");
        f<<lbg<<' '<<first<<'\n'<<cat<<' '<<second<<"\n0 "<<second<<"\n1 999\n2 -1\n3 18\n4 "<<first+2<<"\n-1 19\n15 19\ninvalid\n";
    }
    g_s.presets_loaded=false; load_preset_selection(g_s);
    assert(g_s.selected_layout==-1);
    assert(active_layout(g_s,lbg)==first && active_layout(g_s,cat)==second);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) if (p!=lbg && p!=cat) assert(active_layout(g_s,p)==p);
    { std::ofstream f("touch_weapon_presets.txt"); f<<lbg<<' '<<second<<'\n'<<cat<<' '<<first<<'\n'; }
    load_preset_selection(g_s); assert(active_layout(g_s,lbg)==first && active_layout(g_s,cat)==second);
    in_quest=true; weapon_override=6; profile=lbg; g_s.loaded=false; update();
    assert(g_s.layout_profile==first && control_profile(g_s)==lbg && profile==lbg);
    g_s.presets_loaded=false; g_s.loaded=false; update();
    assert(g_s.layout_profile==lbg && active_layout(g_s,cat)==cat && profile==lbg);
    char missing[40]; layout_path(LAYOUT_SLOTS-1,missing,sizeof missing); assert(remove(missing)==0);
    { std::ofstream f("touch_weapon_presets.txt"); f<<lbg<<' '<<LAYOUT_SLOTS-1<<'\n'; }
    g_s.presets_loaded=false; load_preset_selection(g_s); assert(active_layout(g_s,lbg)==lbg);
    assert(remove("touch_weapon_presets.txt")==0); g_s.presets_loaded=false; load_preset_selection(g_s);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==p && active_layout(g_s,p)==p);
    assert(g_s.selected_layout==-1);
    puts("PASS: preset mappings load once, survive a normal reload and reject invalid indices, missing files and weapon-binding mismatches by falling back to each fixed base");
}
static void reserved_quest_combos() {
    preset_fixture(); const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15),first=RESERVED_LAYOUT_BASE,second=first+1,entry=TOUCH_WEAPON_LAYOUTS+1;
    assert(save_layout(defaults(lbg),first) && save_layout(defaults(cat),second));
    g_s.weapon_layout[lbg]=first; g_s.weapon_layout[cat]=second;
    strcpy(cfg.touch_weapon_combo[entry][0],"X"); strcpy(cfg.touch_weapon_combo[lbg][0],"X+A"); strcpy(cfg.touch_weapon_combo[cat][0],"A");
    touch_edit_begin(); update(); editor_page(first);
    assert(control_profile(g_s)==lbg && profile==entry && in_editor(C_COMBO,false,control_profile(g_s)));
    char label[32]; combo_label(C_COMBO,control_profile(g_s),label,sizeof label); assert(!strcmp(label,"X+A"));
    Finger f{}; f.ctl=C_COMBO; assert(finger_buttons(f)==B(BTN_X));
    editor_click(E_CANCEL); update();
    for (int weapon : {6,15}) {
        const int logical=profile_for_weapon(weapon),slot=weapon==6?first:second;
        in_quest=true; weapon_override=weapon; ranged=weapon==6?WEAPON_BOWGUN:0; drawn=1; update();
        assert(touch_preset_use(weapon==6?16:17,-1));
        assert(g_s.layout_profile==slot && control_profile(g_s)==logical && profile==logical);
        assert(g_ranged==ranged_layout_profile(logical) && finger_buttons(f)==(weapon==6?B(BTN_X)|B(BTN_A):B(BTN_A)));
        const int control=weapon==6?C_RFIRE:C_HEAVY;
        at(SDL_EVENT_FINGER_DOWN,control); assert(update().b==B(BTN_A)); at(SDL_EVENT_FINGER_CANCELED,control);
        touch_edit_begin(); update(); const int binding=control_profile(g_s);
        editor_click(E_SET); editor_click(E_STYLE); update();
        assert(g_s.edit && g_s.layout_profile==slot && control_profile(g_s)==binding && profile==logical);
        g_s.layout.x[C_DPAD_TOGGLE]=.601f; editor_click(E_SAVE); update();
        assert(!g_s.edit && g_s.layout_profile==slot && profile==logical && active_layout(g_s,logical)==slot);
        Layout saved; assert(load_layout(&saved,slot) && fabsf(saved.x[C_DPAD_TOGGLE]-.601f)<.0001f);
    }
    in_quest=false; update(); assert(profile==entry && g_s.layout_profile==entry);
    puts("PASS: type 6 LBG and type 15 Prowler use selected slot positions but original logical combo bindings; quest editing saves without switching or rebinding");
}
static void reserved_scene_autosave() {
    preset_fixture(3); const int slot=RESERVED_LAYOUT_BASE,lbg=profile_for_weapon(6),cat=profile_for_weapon(15);
    assert(save_layout(defaults(lbg),slot)); g_s.weapon_layout[lbg]=slot;
    touch_edit_begin(); update(); editor_page(slot); g_s.layout.x[C_DPAD_TOGGLE]=.743f;
    in_quest=true; weapon_override=15; update();
    assert(!g_s.edit && g_s.layout_profile==cat && profile==cat);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==lbg?slot:p));
    Layout saved; assert(load_layout(&saved,slot) && saved.weapon_profile==lbg && fabsf(saved.x[C_DPAD_TOGGLE]-.743f)<.0001f);
    in_quest=false; update(); touch_edit_begin(); update(); editor_page(slot); g_s.layout.x[C_DPAD_TOGGLE]=.852f;
    in_quest=true; weapon_override=6; update();
    assert(!g_s.edit && g_s.layout_profile==slot && profile==lbg);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==lbg?slot:p));
    assert(load_layout(&saved,slot) && saved.weapon_profile==lbg && fabsf(saved.x[C_DPAD_TOGGLE]-.852f)<.0001f);
    puts("PASS: a scene transition autosaves coordinates of a confirmed reserved binding without changing any weapon's selected preset");
}
static void reserved_binding_drafts() {
    preset_fixture(3); const int slot=RESERVED_LAYOUT_BASE,lbg=profile_for_weapon(6),cat=profile_for_weapon(15),entry=TOUCH_WEAPON_LAYOUTS+3;
    Layout original=defaults(lbg); original.x[C_DPAD_TOGGLE]=.643f;
    assert(save_layout(original,slot)); g_s.weapon_layout[lbg]=slot;
    touch_edit_begin(); update(); editor_page(slot); editor_bind(cat); g_s.layout.x[C_DPAD_TOGGLE]=.931f;
    editor_click(E_SET); update(); assert(g_s.layout_profile==slot+1 && profile==entry);
    Layout saved; assert(load_layout(&saved,slot)); same_layout(saved,original);
    assert(active_layout(g_s,lbg)==slot && active_layout(g_s,cat)==cat);
    editor_page(slot); editor_bind(cat); g_s.layout.x[C_DPAD_TOGGLE]=.942f;
    in_quest=true; weapon_override=15; update();
    assert(!g_s.edit && g_s.layout_profile==cat && profile==cat);
    assert(load_layout(&saved,slot)); same_layout(saved,original);
    assert(active_layout(g_s,lbg)==slot && active_layout(g_s,cat)==cat);
    in_quest=false; update(); touch_edit_begin(); update(); editor_page(slot+1); editor_bind(cat);
    editor_click(E_SET); update(); assert(load_layout(&saved,slot+1) && saved.weapon_profile==-1);
    editor_page(slot+1); editor_bind(cat); g_s.layout.x[C_DPAD_TOGGLE]=.953f;
    in_quest=true; weapon_override=6; update();
    assert(!g_s.edit && g_s.layout_profile==slot && profile==lbg);
    assert(load_layout(&saved,slot+1) && saved.weapon_profile==-1);
    assert(load_layout(&saved,slot)); same_layout(saved,original);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==(p==lbg?slot:p));
    puts("PASS: Next and scene changes discard unconfirmed first-binding/rebinding drafts, preserving the original slot and every weapon's selection");
}
static void editor_save_failure() {
    preset_fixture(); const int slot=RESERVED_LAYOUT_BASE,lbg=profile_for_weapon(6),entry=TOUCH_WEAPON_LAYOUTS+1;
    const Layout original=defaults(lbg); assert(save_layout(original,slot)); g_s.weapon_layout[lbg]=slot;
    touch_edit_begin(); update(); editor_page(slot); g_s.layout.x[C_DPAD_TOGGLE]=.784f;
    char path[40],temp[48]; layout_path(slot,path,sizeof path); snprintf(temp,sizeof temp,"%s.tmp",path);
    assert(!SDL_GetPathInfo(temp,nullptr));  // Never replace or remove a pre-existing path.
    assert(SDL_CreateDirectory(temp));
    editor_click(E_SET); update();
    assert(g_s.edit && g_s.layout_profile==slot && g_s.save_failed && profile==entry && active_layout(g_s,lbg)==slot);
    Layout saved; assert(load_layout(&saved,slot)); same_layout(saved,original);
    SDL_PathInfo info{}; assert(SDL_GetPathInfo(temp,&info) && info.type==SDL_PATHTYPE_DIRECTORY);
    assert(SDL_RemovePath(temp));  // Only the empty directory created by this test.
    editor_click(E_SET); update(); assert(g_s.edit && g_s.layout_profile==slot+1 && profile==entry);
    assert(load_layout(&saved,slot) && fabsf(saved.x[C_DPAD_TOGGLE]-.784f)<.0001f);
    editor_click(E_CANCEL); update(); assert(!g_s.edit);
    puts("PASS: a blocked layout temp path keeps the editor on its current page and preserves the old file; removing the test's empty blocker permits retry");
}
static void direct_preset_selection() {
    preset_fixture(3); const int entry=TOUCH_WEAPON_LAYOUTS+3,cat=profile_for_weapon(15),lbg=profile_for_weapon(6);
    assert(TOUCH_PRESET_COUNT==20 && TOUCH_UNIVERSAL_PRESET==15);
    assert(g_s.selected_layout==-1 && touch_preset_current()==-1);
    for (int p=0;p<TOUCH_PRESET_COUNT;++p) {
        assert(preset_slot(p)==(p<15?p:p+4));
        assert(!strcmp(touch_preset_name(p),profile_name(preset_slot(p))));
        assert(touch_preset_binding(p)==(p<15?p:-1));
    }
    assert(!touch_preset_use(-1,-1) && !touch_preset_use(20,-1));
    assert(!touch_preset_use(0,0) && !touch_preset_use(16,15) && !touch_preset_use(16,-1));
    assert(remove("touch_layout_universal.txt")==0);
    assert(touch_preset_use(TOUCH_UNIVERSAL_PRESET,-1));
    assert(g_s.layout_profile==entry && profile==entry && !g_s.edit && g_s.selected_layout==UNIVERSAL_LAYOUT && touch_preset_current()==15);
    Layout layout; assert(load_layout(&layout,UNIVERSAL_LAYOUT) && layout.style==STYLE_CLASSIC);
    State restored{}; load_preset_selection(restored);
    assert(restored.selected_layout==UNIVERSAL_LAYOUT);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(active_layout(restored,p)==UNIVERSAL_LAYOUT);
    in_quest=true; weapon_override=15; update();
    assert(g_s.layout_profile==UNIVERSAL_LAYOUT && profile==cat && g_style==STYLE_CLASSIC);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) {
        assert(touch_preset_use(p,-1));
        assert(g_s.layout_profile==p && profile==p && g_s.selected_layout==p && touch_preset_current()==p && g_s.detected_weapon==15);
        for (int weapon:{6,15}) {
            weapon_override=weapon; ranged=weapon==6?WEAPON_BOWGUN:0;
            for (int frame=0;frame<3;++frame) update();
            assert(g_s.layout_profile==p && profile==p && control_profile(g_s)==p && g_s.detected_weapon==weapon);
        }
        assert(active_layout(g_s,p)==p);
        for (int q=p+1;q<TOUCH_WEAPON_LAYOUTS;++q) assert(active_layout(g_s,q)==UNIVERSAL_LAYOUT);
    }
    assert(g_s.layout_profile==cat && profile==cat && touch_preset_current()==cat);
    assert(touch_preset_use(TOUCH_UNIVERSAL_PRESET,-1));
    for (int p=16;p<20;++p) {
        assert(touch_preset_use(p,lbg));
        assert(touch_preset_binding(p)==lbg && active_layout(g_s,lbg)==preset_slot(p));
        assert(load_layout(&layout,preset_slot(p)) && layout.style==STYLE_CLASSIC);
        assert(g_s.layout_profile==preset_slot(p) && profile==lbg && control_profile(g_s)==lbg && touch_preset_current()==p);
        for (int frame=0;frame<3;++frame) update();
        assert(g_s.layout_profile==preset_slot(p) && profile==lbg && g_s.detected_weapon==15);
    }
    assert(touch_preset_use(16,cat));
    assert(g_s.layout_profile==RESERVED_LAYOUT_BASE && profile==cat);
    assert(touch_preset_use(16,lbg));  // Rebinding changes the source semantics, never the real weapon or selected slot.
    assert(g_s.layout_profile==RESERVED_LAYOUT_BASE && profile==lbg && g_s.detected_weapon==15 && active_layout(g_s,lbg)==RESERVED_LAYOUT_BASE);
    assert(load_layout(&layout,RESERVED_LAYOUT_BASE) && layout.weapon_profile==lbg);
    assert(touch_preset_use(18,-1) && active_layout(g_s,lbg)==preset_slot(18));
    assert(g_s.layout_profile==preset_slot(18) && profile==lbg && touch_preset_current()==18);
    in_quest=false; update(); assert(g_s.layout_profile==entry && profile==entry && touch_preset_current()==18);
    in_quest=true; weapon_override=0; ranged=0; update();
    assert(g_s.layout_profile==preset_slot(18) && profile==lbg && g_s.detected_weapon==0);
    puts("PASS: all 20 presets apply directly to the current quest regardless of its real weapon, retain their source semantics through frames/weapon changes, preserve legacy mappings, and return after ordinary village layouts");
}
static void direct_preset_reapply() {
    const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15);
    const int presets[] = {lbg,TOUCH_UNIVERSAL_PRESET,16};
    for (int preset:presets) {
        preset_fixture(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
        const int slot=preset_slot(preset);
        Layout initial=defaults(slot);
        if (reserved_layout(slot)) {
            initial=defaults(lbg); initial.style=STYLE_CLASSIC; initial.y[C_DPAD_TOGGLE]=.70f;
        }
        assert(save_layout(initial,slot));
        in_quest=true; weapon_override=6; ranged=WEAPON_BOWGUN; drawn=1; update();
        assert(touch_preset_use(preset,-1));
        assert(g_s.layout_profile==slot && profile==lbg && !g_s.edit);
        assert(g_style==initial.style && g_ranged==(initial.style==STYLE_CLASSIC?0:WEAPON_BOWGUN));
        Layout edited=g_s.layout;
        edited.x[C_L]=.1732f; edited.y[C_L]=.7712f; edited.scale[C_L]=1.213f;
        edited.x[C_STICK]=.3712f; edited.y[C_STICK]=.2112f; edited.scale[C_STICK]=1.157f;
        edited.zx[TZ_ITEM]=31.5f; edited.zy[TZ_ITEM]=-14.5f; edited.zx[TZ_TARGET]=-9.5f;
        edited.style=reserved_layout(slot)?STYLE_CORNER:STYLE_CLASSIC;
        if (edited.style==STYLE_CLASSIC) edited.y[C_DPAD_TOGGLE]=.70f;
        assert(save_layout(edited,slot)); const std::string on_disk=stored_layout_text(slot);
        int selected[TOUCH_WEAPON_LAYOUTS]; std::copy(std::begin(g_s.weapon_layout),std::end(g_s.weapon_layout),selected);
        const Geo old_l=place(g_s.layout,C_L,(float)screen_w,(float)screen_h);
        const Geo old_stick=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        at(SDL_EVENT_FINGER_DOWN,C_L,1); at(SDL_EVENT_FINGER_DOWN,C_ZL,2); at(SDL_EVENT_FINGER_UP,C_ZL,2);
        assert(pulses(B(BTN_ZL))==1);
        // Keep a real tap's pending pulse alive independently of host disk timing.
        for (Pulse &p:g_s.pulse) if (p.buttons==B(BTN_ZL)) p.until=SDL_GetTicksNS()+2000*MS;
        at(SDL_EVENT_FINGER_DOWN,C_STICK,3,.6f);
        const Input held=update(); assert(held.b==(B(BTN_L)|B(BTN_ZL)) && held.lx>15000 && g_s.stick_on);
        event(SDL_EVENT_FINGER_DOWN,4,screen_w*.5f,screen_h*.5f);
        assert(finger(g_s,4) && finger(g_s,4)->kind==F_CAMERA);
        event(SDL_EVENT_FINGER_MOTION,4,screen_w*.5f+32,screen_h*.5f-21);
        assert(g_s.cam_dx>0 && g_s.cam_dy<0);
        assert(touch_preset_use(preset,-1));
        assert(g_s.layout_profile==slot && profile==lbg && !g_s.edit);
        same_layout(g_s.layout,edited); same_layout(g_s.saved,edited);
        assert(g_style==edited.style && g_ranged==(edited.style==STYLE_CLASSIC?0:WEAPON_BOWGUN));
        for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(g_s.weapon_layout[p]==selected[p]);
        for (int z=0;z<TZ_COUNT;++z) assert(g_touch_zone_dx[z]==edited.zx[z] && g_touch_zone_dy[z]==edited.zy[z]);
        for (const Finger &f:g_s.f) assert(f.kind==F_NONE);
        for (const Pulse &p:g_s.pulse) assert(!p.buttons && !p.from && !p.until && !p.dpad);
        assert(!g_s.held && !g_s.stick_on && !g_s.run && !g_s.cam_dx && !g_s.cam_dy && !g_s.ammo_ready && !g_s.ammo_hold);
        assert(stored_layout_text(slot)==on_disk);
        const Input physical=update_axes(B(BTN_A),7319,-4178,12345,-23456);
        assert(physical.b==B(BTN_A) && !physical.touch_b && physical.lx==7319 && physical.ly==-4178 && physical.rx==12345 && physical.ry==-23456 && !physical.dx && !physical.dy);
        event(SDL_EVENT_FINGER_UP,1,old_l.c.x,old_l.c.y);
        event(SDL_EVENT_FINGER_UP,3,old_stick.c.x+old_stick.r*.6f,old_stick.c.y);
        event(SDL_EVENT_FINGER_UP,4,screen_w*.5f+32,screen_h*.5f-21);
        const Input released=update();
        assert(!released.b && !released.touch_b && !released.lx && !released.ly && !released.rx && !released.ry && !released.dx && !released.dy);
        assert(touch_preset_use(cat,-1));  // Direct use selects another source even while the actual weapon is still LBG.
        assert(g_s.layout_profile==cat && profile==cat && g_s.detected_weapon==6);
        weapon_override=15; ranged=0; update(); assert(g_s.layout_profile==cat && profile==cat);
        Layout saved; assert(load_layout(&saved,slot)); same_layout(saved,edited); assert(stored_layout_text(slot)==on_disk);
        assert(touch_preset_use(preset,-1));
        weapon_override=6; ranged=WEAPON_BOWGUN; update();
        assert(g_s.layout_profile==slot && profile==lbg); same_layout(g_s.layout,edited);
        assert(g_style==edited.style && g_ranged==(edited.style==STYLE_CLASSIC?0:WEAPON_BOWGUN));
    }
    puts("PASS: reapplying an already loaded fixed/universal/bound reserved preset reloads newer disk position, size, HUD and compatible style, clears old fingers/pulses/axes/camera, preserves physical input and legacy mappings, and switching to another source/weapon retains the updated file");
}
static void direct_preset_failure() {
    preset_fixture(); const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15),slot=RESERVED_LAYOUT_BASE;
    assert(touch_preset_use(16,lbg));
    assert(g_s.selected_layout==slot && touch_preset_current()==16);
    Layout old; assert(load_layout(&old,slot));
    std::ifstream map("touch_weapon_presets.txt");
    const std::string original((std::istreambuf_iterator<char>(map)),{}); map.close();
    const char *temp="touch_weapon_presets.txt.tmp";
    assert(!SDL_GetPathInfo(temp,nullptr) && SDL_CreateDirectory(temp));
    assert(!touch_preset_use(16,cat));
    assert(g_s.selected_layout==slot && touch_preset_current()==16);
    Layout current; assert(load_layout(&current,slot)); same_layout(current,old);
    assert(active_layout(g_s,lbg)==slot && active_layout(g_s,cat)==cat);
    assert(remove("touch_layout_universal.txt")==0);
    assert(!touch_preset_use(TOUCH_UNIVERSAL_PRESET,-1));
    assert(g_s.selected_layout==slot && touch_preset_current()==16);
    assert(!load_layout(&current,UNIVERSAL_LAYOUT));
    std::ifstream after("touch_weapon_presets.txt");
    assert(std::string((std::istreambuf_iterator<char>(after)),{})==original); after.close();
    assert(SDL_RemovePath(temp));
    assert(touch_preset_use(16,cat));
    assert(g_s.selected_layout==slot && touch_preset_current()==16);
    assert(active_layout(g_s,lbg)==lbg && active_layout(g_s,cat)==slot);
    puts("PASS: direct-selection commit failure preserves mappings and prior reserved contents, removes uncommitted new universal file, and allows retry");
}
static void directed_preset_editor() {
    preset_fixture(3); const int entry=TOUCH_WEAPON_LAYOUTS+3;
    for (int p=0;p<TOUCH_PRESET_COUNT;++p) {
        if (p>=16) assert(touch_preset_use(p,p-16));
        settings_open=true; touch_edit_preset(p); update();
        assert(!g_s.edit && g_s.edit_request && g_s.layout_profile==entry);
        settings_open=false; update();
        assert(g_s.edit && !g_s.edit_request && g_s.layout_profile==preset_slot(p) && profile==entry);
        g_s.layout.x[C_MENU]=.51f+p*.001f;
        editor_click(E_SAVE); update();
        assert(!g_s.edit && g_s.layout_profile==entry && profile==entry && g_s.selected_layout==preset_slot(p) && touch_preset_current()==p);
        Layout saved; assert(load_layout(&saved,preset_slot(p)));
        assert(fabsf(saved.x[C_MENU]-(.51f+p*.001f))<.0001f);
    }
    settings_open=true; touch_edit_preset(15); touch_edit_cancel(); settings_open=false; update();
    assert(!g_s.edit && !g_s.edit_request && g_s.edit_profile_request==-1 && g_s.layout_profile==entry);
    touch_edit_preset(15); touch_edit_begin(); update();
    assert(g_s.edit && g_s.layout_profile==entry); editor_click(E_CANCEL); update();
    assert(touch_preset_use(15,-1));
    touch_edit_preset(15); update(); g_s.layout.x[C_MENU]=.99f;
    editor_click(E_CANCEL); update();
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(active_layout(g_s,p)==UNIVERSAL_LAYOUT);
    in_quest=true; weapon_override=15; update();
    touch_edit_preset(15); update(); assert(g_s.edit && g_s.layout_profile==UNIVERSAL_LAYOUT);
    editor_click(E_SET); update(); assert(g_s.layout_profile==UNIVERSAL_LAYOUT);
    g_s.layout.x[C_MENU]=.718f; editor_click(E_SAVE); update();
    Layout saved; assert(!g_s.edit && load_layout(&saved,UNIVERSAL_LAYOUT) && fabsf(saved.x[C_MENU]-.718f)<.0001f);
    assert(profile==profile_for_weapon(15) && g_s.layout_profile==UNIVERSAL_LAYOUT);
    puts("PASS: directed editor opens every weapon/universal/reserved slot only after menu close; Save persists in and out of quests, Cancel clears requests without undoing explicit preset selection");
}
static void manual_preset_source_controls() {
    const int lbg=profile_for_weapon(6),bow=profile_for_weapon(10);
    struct Source { int preset,source,actual; const char *combo; uint64_t buttons; };
    const Source sources[]={{0,0,6,"A",B(BTN_A)},{lbg,lbg,15,"X+A",B(BTN_X)|B(BTN_A)},
                            {16,bow,0,"X",B(BTN_X)}};
    for (const Source &c:sources) {
        preset_fixture(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
        const int slot=preset_slot(c.preset),actual=profile_for_weapon(c.actual);
        Layout source=defaults(c.source);
        source.x[C_COMBO]=.88f; source.y[C_COMBO]=.50f; source.scale[C_COMBO]=1.05f;
        source.x[C_DPAD_TOGGLE]=.5123f; source.y[C_DPAD_TOGGLE]=.6123f; source.scale[C_DPAD_TOGGLE]=1.217f;
        source.zx[TZ_ITEM]=27.5f; source.zy[TZ_TARGET]=-13.5f;
        assert(save_layout(source,slot)); const std::string original=stored_layout_text(slot);
        strcpy(cfg.touch_weapon_combo[actual][0],strcmp(c.combo,"A")?"A":"X");
        strcpy(cfg.touch_weapon_combo[c.source][0],c.combo);
        in_quest=true; weapon_override=c.actual; ranged=ranged_profile(actual); drawn=1; update();
        assert(touch_preset_use(c.preset,-1));
        assert(g_s.layout_profile==slot && g_s.selected_layout==slot && control_profile(g_s)==c.source && profile==c.source);
        assert(g_s.detected_weapon==c.actual && g_style==source.style && g_ranged==ranged_profile(c.source));
        same_layout(g_s.layout,source);
        char label[32]; combo_label(C_COMBO,control_profile(g_s),label,sizeof label); assert(!strcmp(label,c.combo));
        for (int weapon:{0,6,15}) {
            weapon_override=weapon; ranged=ranged_profile(profile_for_weapon(weapon));
            for (int frame=0;frame<3;++frame) update();
            assert(g_s.layout_profile==slot && profile==c.source && g_s.detected_weapon==weapon && touch_preset_current()==c.preset);
            assert(g_ranged==ranged_profile(c.source)); same_layout(g_s.layout,source);
            at(SDL_EVENT_FINGER_DOWN,C_COMBO); const Input pressed=update();
            assert(pressed.b==c.buttons && pressed.touch_b==c.buttons);
            at(SDL_EVENT_FINGER_CANCELED,C_COMBO); assert(!update().b);
        }
        game_menu_state(true); assert(g_s.layout_profile==slot && profile==c.source);
        game_menu_state(false); assert(g_s.layout_profile==slot && profile==c.source);
        touch_reset(); g_s={}; update();
        assert(g_s.layout_profile==slot && g_s.selected_layout==slot && profile==c.source && touch_preset_current()==c.preset);
        same_layout(g_s.layout,source);
        assert(config_live_touch_combo(profile,0)==c.buttons && stored_layout_text(slot)==original);
    }
    puts("PASS: cross-weapon fixed/reserved presets keep their own coordinates, size, HUD, attack/ranged semantics and combo labels/output through frames, real weapon changes, menus and restart without rewriting the source file");
}
static void manual_preset_persistence() {
    preset_fixture(3); const int entry=TOUCH_WEAPON_LAYOUTS+3,lbg=profile_for_weapon(6),cat=profile_for_weapon(15),slot=RESERVED_LAYOUT_BASE;
    Layout source=defaults(lbg); source.x[C_DPAD_TOGGLE]=.6123f; source.scale[C_DPAD_TOGGLE]=1.231f; source.zx[TZ_ITEM]=18.5f;
    assert(save_layout(source,slot)); const std::string original=stored_layout_text(slot);
    strcpy(cfg.touch_weapon_combo[lbg][0],"X+A"); strcpy(cfg.touch_weapon_combo[cat][0],"A");
    assert(g_s.selected_layout==-1 && touch_preset_current()==-1);
    assert(touch_preset_use(16,-1));
    assert(g_s.layout_profile==entry && profile==entry && g_s.selected_layout==slot && touch_preset_current()==16);
    State restored{}; load_preset_selection(restored);
    assert(restored.selected_layout==slot && quest_layout(restored,cat)==slot && active_layout(restored,cat)==cat);
    in_quest=true; weapon_override=UNKNOWN_WEAPON;
    for (int frame=0;frame<3;++frame) {
        update(); assert(g_s.layout_profile==slot && profile==lbg && g_s.detected_weapon==-1 && touch_preset_current()==16);
        assert(config_live_touch_combo(profile,0)==(B(BTN_X)|B(BTN_A))); same_layout(g_s.layout,source);
    }
    weapon_override=15; update();
    assert(g_s.layout_profile==slot && profile==lbg && g_s.detected_weapon==15); same_layout(g_s.layout,source);
    assert(config_live_touch_combo(profile,0)==(B(BTN_X)|B(BTN_A)));
    in_quest=false; update(); assert(g_s.layout_profile==entry && profile==entry && touch_preset_current()==16);
    at(SDL_EVENT_FINGER_DOWN,C_LAYOUT); at(SDL_EVENT_FINGER_UP,C_LAYOUT); update();
    assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+1 && g_s.selected_layout==slot);
    in_quest=true; weapon_override=UNKNOWN_WEAPON; update(); assert(g_s.layout_profile==slot && profile==lbg && g_s.detected_weapon==-1);
    touch_reset(); g_s={}; update();
    assert(g_s.layout_profile==slot && profile==lbg && g_s.selected_layout==slot && stored_layout_text(slot)==original);
    assert(g_s.detected_weapon==-1 && config_live_touch_combo(profile,0)==(B(BTN_X)|B(BTN_A)));
    weapon_override=0; update(); assert(g_s.layout_profile==slot && profile==lbg && g_s.detected_weapon==0);
    assert(touch_preset_use(TOUCH_UNIVERSAL_PRESET,-1));
    for (int weapon:{6,15}) {
        weapon_override=weapon; ranged=ranged_profile(profile_for_weapon(weapon)); update();
        assert(g_s.layout_profile==UNIVERSAL_LAYOUT && profile==profile_for_weapon(weapon) && g_ranged==0);
    }
    touch_reset(); g_s={}; update();
    assert(g_s.layout_profile==UNIVERSAL_LAYOUT && g_s.selected_layout==UNIVERSAL_LAYOUT && profile==cat && touch_preset_current()==15);
    // Old numeric mappings remain meaningful when no manual record exists, even after a previously selected save.
    { std::ofstream f("touch_weapon_presets.txt"); f<<lbg<<' '<<slot<<'\n'; }
    State legacy{}; load_preset_selection(legacy);
    assert(legacy.selected_layout==-1 && quest_layout(legacy,lbg)==slot && quest_layout(legacy,cat)==cat);
    char missing[40]; layout_path(LAYOUT_SLOTS-1,missing,sizeof missing); assert(remove(missing)==0);
    { char corrupt[40]; layout_path(RESERVED_LAYOUT_BASE+2,corrupt,sizeof corrupt); std::ofstream f(corrupt); f<<"weapon_profile 99 0 1\n"; }
    const char *invalid[]={"-1","-99","15","16","17","18","24","999","bad","21","22","23"};
    for (const char *record:invalid) {
        { std::ofstream f("touch_weapon_presets.txt"); f<<lbg<<' '<<slot<<"\nselected "<<record<<'\n'; }
        State bad{}; load_preset_selection(bad);
        assert(bad.selected_layout==-1 && quest_layout(bad,lbg)==slot && quest_layout(bad,cat)==cat);
    }
    char universal[40]; layout_path(UNIVERSAL_LAYOUT,universal,sizeof universal); assert(remove(universal)==0);
    { std::ofstream f("touch_weapon_presets.txt"); f<<lbg<<' '<<slot<<"\nselected "<<UNIVERSAL_LAYOUT<<'\n'; }
    State missing_universal{}; load_preset_selection(missing_universal);
    assert(missing_universal.selected_layout==-1 && quest_layout(missing_universal,lbg)==slot);
    g_s.presets_loaded=false; g_s.loaded=false; weapon_override=15; update();
    assert(g_s.selected_layout==-1 && g_s.layout_profile==cat && profile==cat && touch_preset_current()==cat);
    puts("PASS: a manual quest selection persists independently of ordinary village layouts and real weapons, restores reserved source/combos before weapon detection and after restart, and keeps universal actual-weapon combo background; old mappings and damaged/legacy/missing/unbound selected records fall back safely");
}
static void manual_preset_editor_selection() {
    preset_fixture(); const int lbg=profile_for_weapon(6),cat=profile_for_weapon(15),bow=profile_for_weapon(10);
    strcpy(cfg.touch_weapon_combo[lbg][0],"X"); strcpy(cfg.touch_weapon_combo[cat][0],"A"); strcpy(cfg.touch_weapon_combo[bow][0],"X+A");
    assert(touch_preset_use(lbg,-1));
    in_quest=true; weapon_override=0; update(); assert(g_s.layout_profile==lbg && profile==lbg);
    Layout original; assert(load_layout(&original,cat));
    touch_edit_preset(cat); update(); assert(g_s.edit && g_s.layout_profile==cat && profile==lbg && g_s.selected_layout==lbg);
    g_s.layout.x[C_DPAD_TOGGLE]=.6432f;
    const char *temp="touch_weapon_presets.txt.tmp";
    assert(!SDL_GetPathInfo(temp,nullptr) && SDL_CreateDirectory(temp));
    editor_click(E_SAVE); update();
    assert(g_s.edit && g_s.save_failed && g_s.selected_layout==lbg && profile==lbg && touch_preset_current()==lbg);
    Layout saved; assert(load_layout(&saved,cat)); same_layout(saved,original);
    State unchanged{}; load_preset_selection(unchanged); assert(unchanged.selected_layout==lbg);
    assert(SDL_RemovePath(temp)); editor_click(E_CANCEL); update();
    assert(!g_s.edit && g_s.layout_profile==lbg && g_s.selected_layout==lbg && profile==lbg);
    touch_edit_preset(cat); update(); g_s.layout.x[C_DPAD_TOGGLE]=.6432f; editor_click(E_SAVE); update();
    assert(!g_s.edit && g_s.layout_profile==cat && g_s.selected_layout==cat && profile==cat && g_s.detected_weapon==0 && touch_preset_current()==cat);
    at(SDL_EVENT_FINGER_DOWN,C_COMBO); assert(update().touch_b==B(BTN_A)); at(SDL_EVENT_FINGER_CANCELED,C_COMBO); assert(!update().b);
    assert(load_layout(&saved,cat) && fabsf(saved.x[C_DPAD_TOGGLE]-.6432f)<.0001f);
    touch_edit_preset(lbg); update(); g_s.layout.x[C_DPAD_TOGGLE]=.9876f; editor_click(E_CANCEL); update();
    assert(!g_s.edit && g_s.layout_profile==cat && profile==cat && g_s.selected_layout==cat);
    touch_edit_preset(bow); update(); g_s.layout.x[C_DPAD_TOGGLE]=.5678f;
    weapon_override=6; ranged=WEAPON_BOWGUN; update();
    assert(!g_s.edit && g_s.layout_profile==cat && profile==cat && g_s.selected_layout==cat && g_s.detected_weapon==6);
    assert(load_layout(&saved,bow) && fabsf(saved.x[C_DPAD_TOGGLE]-.5678f)<.0001f);
    State restored{}; load_preset_selection(restored); assert(restored.selected_layout==cat);
    puts("PASS: editor preview/Cancel and scene autosave preserve the manual quest selection, Save and use selects its source independently of the real weapon, and a failed selection commit rolls back the file and retains live/persisted selection");
}
static void classic_fixture(int w=1280,int h=720) {
    fresh(w,h); assert(STYLE_CLASSIC==STYLE_COUNT && STYLE_COUNT==3);
    assert(save_layout(defaults(UNIVERSAL_LAYOUT),UNIVERSAL_LAYOUT));
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) g_s.weapon_layout[p]=UNIVERSAL_LAYOUT;
    g_s.detected_weapon=-1; update();
    assert(g_s.layout_profile==UNIVERSAL_LAYOUT && g_s.layout.style==STYLE_CLASSIC && g_ranged==0 && profile==0);
}
static void classic_variant_fixture(int slot,int w=1280,int h=720) {
    classic_fixture(w,h);
    if (reserved_layout(slot)) {
        Layout inherited=g_s.layout; inherited.weapon_profile=0; assert(save_layout(inherited,slot));
        g_s.weapon_layout[0]=slot; g_s.detected_weapon=-1; update();
    }
    assert(g_s.layout_profile==slot && g_s.layout.style==STYLE_CLASSIC && profile==0);
}
static void classic_shortcut_compatibility() {
    const Layout current=defaults(UNIVERSAL_LAYOUT);
    assert(current.x[C_DPAD_TOGGLE]==.18f && current.y[C_DPAD_TOGGLE]==.70f && current.scale[C_DPAD_TOGGLE]==1);
    for (const ImVec2 size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080),ImVec2(640,360)}) {
        const Geo toggle=place(current,C_DPAD_TOGGLE,size.x,size.y);
        for (int c:{C_L,C_ZL,C_SETTINGS,C_HDPAD}) {
            const Geo other=place(current,c,size.x,size.y);
            assert(hypotf(toggle.c.x-other.c.x,toggle.c.y-other.c.y)>toggle.r+other.r);
        }
    }
    for (int slot=UNIVERSAL_LAYOUT;slot<LAYOUT_SLOTS;++slot) {
        Layout legacy=current; legacy.weapon_profile=reserved_layout(slot)?0:-1;
        legacy.x[C_DPAD_TOGGLE]=DEFS[C_DPAD_TOGGLE].x; legacy.y[C_DPAD_TOGGLE]=DEFS[C_DPAD_TOGGLE].y;
        legacy.x[C_L]=.147f; legacy.x[C_RSTICK]=.381f; legacy.scale[C_RSTICK]=1.23f; legacy.zx[TZ_ITEM]=12.5f;
        assert(save_layout(legacy,slot)); const std::string original=stored_layout_text(slot);
        Layout expected=legacy,loaded; expected.x[C_DPAD_TOGGLE]=current.x[C_DPAD_TOGGLE]; expected.y[C_DPAD_TOGGLE]=current.y[C_DPAD_TOGGLE];
        assert(load_layout(&loaded,slot)); same_layout(loaded,expected); assert(stored_layout_text(slot)==original);
        const float custom[][3]={{.245f,DEFS[C_DPAD_TOGGLE].y,1},{DEFS[C_DPAD_TOGGLE].x,.745f,1},
                                 {DEFS[C_DPAD_TOGGLE].x,DEFS[C_DPAD_TOGGLE].y,1.25f},{.245f,.65f,.75f}};
        for (const auto &p:custom) {
            Layout edited=legacy; edited.x[C_DPAD_TOGGLE]=p[0]; edited.y[C_DPAD_TOGGLE]=p[1]; edited.scale[C_DPAD_TOGGLE]=p[2];
            assert(save_layout(edited,slot)); const std::string before=stored_layout_text(slot);
            assert(load_layout(&loaded,slot)); same_layout(loaded,edited); assert(stored_layout_text(slot)==before);
        }
    }
    Layout normal=defaults(0),loaded; assert(save_layout(normal,0) && load_layout(&loaded,0)); same_layout(normal,loaded);
    puts("PASS: classic shortcut defaults to a clear position at all four screen sizes; old unused defaults migrate only in memory, custom position/size and every other key stay intact in universal/all inherited classic files; normal layouts do not migrate");
}
static void classic_show_settings() {
    struct Shown { int setting,ctl; uint64_t button; float ux; };
    const Shown controls[]={{TB_ATTACKS,C_HEAVY,B(BTN_A),0},{TB_ATTACKS,C_LIGHT,B(BTN_X),0},
        {TB_DODGE,C_DODGE,B(BTN_B),0},{TB_SHEATHE,C_SHEATHE,B(BTN_Y),0},{TB_GUARD,C_R,B(BTN_R),0},
        {TB_L,C_L,B(BTN_L),0},{TB_ZL,C_ZL,B(BTN_ZL),0},{TB_ZR,C_ZR,B(BTN_ZR),0},
        {TB_PLUS,C_MENU,B(BTN_PLUS),0},{TB_MINUS,C_HMINUS,B(BTN_MINUS),0},
        {TB_L3,C_MAP,B(BTN_LSTICK),0},{TB_R3,C_R3,B(BTN_RSTICK),0},
        {TB_DPAD,C_HDPAD,DPAD_RIGHT,.7f},{TB_STICK,C_STICK,0,.6f}};
    for (int slot=UNIVERSAL_LAYOUT;slot<LAYOUT_SLOTS;++slot) for (int menu=0;menu<2;++menu) {
        classic_variant_fixture(slot); game_menu_state(menu!=0); const Layout original=g_s.layout;
        for (const Shown &c:controls) {
            touch_reset(); g_s.layout=g_s.saved=original;
            std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); cfg.touch_show[c.setting]=1;
            // Isolate native drawing/hits from neighboring default controls without changing saved files.
            const float x=screen_w*.55f,y=screen_h*.5f;
            g_s.layout.x[c.ctl]=(from_right(g_s.layout,c.ctl)?screen_w-x:x)/screen_h; g_s.layout.y[c.ctl]=1-y/screen_h;
            game_menu_state(menu!=0); const Geo g=place(g_s.layout,c.ctl,(float)screen_w,(float)screen_h);
            assert(show_setting(c.ctl)==c.setting && in_mode(c.ctl,g_s.menu,true) && in_editor(c.ctl,true,control_profile(g_s)));
            assert(drawn_vertices(g.c,g.r*.75f)>0);
            at(SDL_EVENT_FINGER_DOWN,c.ctl,1,c.ux); const Input on=update();
            if (c.ctl==C_STICK) assert(on.lx>15000 && !on.ly && !on.touch_b && g_s.stick_on);
            else assert(on.touch_b==c.button && on.b==c.button);
            cfg.touch_show[c.setting]=0;
            const uint64_t physical=c.button?c.button:DPAD_LEFT;
            const Input off=update_axes(physical,7319,-4178,12345,-23456);
            assert(off.b==physical && !off.touch_b && off.lx==7319 && off.ly==-4178 && off.rx==12345 && off.ry==-23456);
            assert(!in_mode(c.ctl,g_s.menu,true) && !in_editor(c.ctl,true,control_profile(g_s)));
            assert(drawn_vertices(g.c,g.r*.75f)==0);
            int part=0; assert(hit(g_s.layout,g.c.x+c.ux*g.r,g.c.y,(float)screen_w,(float)screen_h,&part,1.22f,true,g_s.menu,true,false,control_profile(g_s))!=c.ctl);
            at(SDL_EVENT_FINGER_MOTION,c.ctl,1,c.ux+.1f); at(SDL_EVENT_FINGER_UP,c.ctl,1,c.ux+.1f);
            const Input released=update(); assert(!released.touch_b && !released.b && !released.lx && !released.ly);
            touch_edit_begin(); update(); assert(!in_editor(c.ctl,true,control_profile(g_s)) && drawn_vertices(g.c,g.r*.75f)==0);
            cfg.touch_show[c.setting]=1; update(); assert(in_editor(c.ctl,true,control_profile(g_s)) && drawn_vertices(g.c,g.r*.75f)>0);
            at(SDL_EVENT_FINGER_DOWN,c.ctl,1,c.ux); assert(g_s.sel==c.ctl && finger(g_s,1) && finger(g_s,1)->kind==F_EDIT);
            at(SDL_EVENT_FINGER_CANCELED,c.ctl,1,c.ux); editor_click(E_CANCEL); update(); assert(!g_s.edit && g_s.layout_profile==slot);
        }
        g_s.layout=g_s.saved=original; std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); update();
        assert(in_mode(C_SETTINGS,g_s.menu,true) && in_mode(C_DPAD_TOGGLE,g_s.menu,true) && !in_mode(C_RSTICK,g_s.menu,true));
        for (int c:{C_SETTINGS,C_DPAD_TOGGLE}) {
            const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            assert(drawn_vertices(g.c,g.r*.75f)>0 && in_editor(c,true,control_profile(g_s)));
        }
    }
    puts("PASS: all 13 existing Show mappings, including both attack buttons, gate native drawing, hits and editor selection in universal/all four inherited classic layouts and menus; disabling a held control releases only touch buttons/axes, Settings and D-pad shortcut remain available");
}
static void classic_dpad_shortcut() {
    for (int slot=UNIVERSAL_LAYOUT;slot<LAYOUT_SLOTS;++slot)
    for (const ImVec2 size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080),ImVec2(640,360)}) {
        classic_variant_fixture(slot,(int)size.x,(int)size.y);
        std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); cfg.touch_show[TB_STICK]=1;
        const Geo toggle=place(g_s.layout,C_DPAD_TOGGLE,(float)screen_w,(float)screen_h);
        const Geo dpad=place(g_s.layout,C_HDPAD,(float)screen_w,(float)screen_h);
        for (int menu=0;menu<2;++menu) {
            touch_reset(); cfg.touch_show[TB_DPAD]=0; game_menu_state(menu!=0);
            assert(in_mode(C_DPAD_TOGGLE,g_s.menu,true) && in_editor(C_DPAD_TOGGLE,true,control_profile(g_s)));
            assert(drawn_vertices(toggle.c,toggle.r*.75f)>0 && drawn_vertices(dpad.c,dpad.r*.75f)==0);
            at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE,1); assert(cfg.touch_show[TB_DPAD]); at(SDL_EVENT_FINGER_UP,C_DPAD_TOGGLE,1);
            assert(drawn_vertices(dpad.c,dpad.r*.75f)>0 && in_mode(C_HDPAD,g_s.menu,true));
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,2,.7f); at(SDL_EVENT_FINGER_DOWN,C_STICK,3,.6f);
            const Input on=update(); assert(on.touch_b==DPAD_RIGHT && on.lx>15000 && g_s.stick_on);
            at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE,4); assert(!cfg.touch_show[TB_DPAD]);
            const Input off=update_axes(DPAD_LEFT,1111,-2222,12345,-23456);
            assert(off.b==DPAD_LEFT && !off.touch_b && off.lx>15000 && off.ly==-2222 && off.rx==12345 && off.ry==-23456 && g_s.stick_on);
            assert(!g_s.held && !in_mode(C_HDPAD,g_s.menu,true) && drawn_vertices(dpad.c,dpad.r*.75f)==0);
            at(SDL_EVENT_FINGER_MOTION,C_HDPAD,2,-.7f); at(SDL_EVENT_FINGER_UP,C_HDPAD,2,-.7f);
            at(SDL_EVENT_FINGER_UP,C_DPAD_TOGGLE,4); at(SDL_EVENT_FINGER_CANCELED,C_STICK,3,.6f);
            assert(!update().touch_b && !pulses(DPAD_RIGHT) && !pulses(DPAD_LEFT));
            at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE,5); at(SDL_EVENT_FINGER_UP,C_DPAD_TOGGLE,5);
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,6,0,-.7f); at(SDL_EVENT_FINGER_UP,C_HDPAD,6,0,-.7f);
            assert(cfg.touch_show[TB_DPAD] && pulses(DPAD_UP)==1);
            at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE,7); assert(!cfg.touch_show[TB_DPAD] && !pulses(DPAD_UP));
            assert(update(DPAD_DOWN,true).b==DPAD_DOWN && !g_s.held); at(SDL_EVENT_FINGER_UP,C_DPAD_TOGGLE,7);
            assert(drawn_vertices(toggle.c,toggle.r*.75f)>0 && !update().touch_b);
        }
        touch_reset(); game_menu_state(false); touch_edit_begin(); update();
        assert(g_s.edit && !cfg.touch_show[TB_DPAD] && in_editor(C_DPAD_TOGGLE,true,control_profile(g_s)));
        at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE); assert(g_s.sel==C_DPAD_TOGGLE && finger(g_s,1)->kind==F_EDIT);
        const float dx=.04f*screen_h,dy=.02f*screen_h;
        event(SDL_EVENT_FINGER_MOTION,1,toggle.c.x+dx,toggle.c.y+dy); event(SDL_EVENT_FINGER_UP,1,toggle.c.x+dx,toggle.c.y+dy);
        const Geo moved=place(g_s.layout,C_DPAD_TOGGLE,(float)screen_w,(float)screen_h);
        assert(fabsf(moved.c.x-toggle.c.x-dx)<.1f && fabsf(moved.c.y-toggle.c.y-dy)<.1f);
        editor_click(E_BIGGER); const Layout edited=g_s.layout; assert(edited.scale[C_DPAD_TOGGLE]>1.09f);
        editor_click(E_SAVE); update(); assert(!g_s.edit && g_s.layout_profile==slot);
        Layout saved; assert(load_layout(&saved,slot)); same_layout(saved,edited);
        at(SDL_EVENT_FINGER_DOWN,C_DPAD_TOGGLE,8); at(SDL_EVENT_FINGER_UP,C_DPAD_TOGGLE,8); assert(cfg.touch_show[TB_DPAD]);
    }
    puts("PASS: universal/all four inherited classic D-pad shortcuts stay visible at 16:9/21:9/32:9/640x360, toggle in hunts/menus, clear held/tapped touch directions while preserving physical buttons/axes and touch movement, and their drag/size round-trip saves");
}
static void classic_controls() {
    classic_fixture(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
    struct Button { int ctl; uint64_t button; };
    const Button buttons[]={{C_HEAVY,B(BTN_A)},{C_LIGHT,B(BTN_X)},{C_DODGE,B(BTN_B)},{C_SHEATHE,B(BTN_Y)},
        {C_L,B(BTN_L)},{C_R,B(BTN_R)},{C_ZL,B(BTN_ZL)},{C_ZR,B(BTN_ZR)},
        {C_MENU,B(BTN_PLUS)},{C_HMINUS,B(BTN_MINUS)},{C_MAP,B(BTN_LSTICK)},{C_R3,B(BTN_RSTICK)}};
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) {
        weapon_override=PROFILE_WEAPON[p]; ranged=ranged_profile(p); drawn=p%2; update();
        assert(g_s.layout_profile==UNIVERSAL_LAYOUT && g_s.layout.style==STYLE_CLASSIC && profile==p && g_ranged==0);
        for (int menu=0;menu<2;++menu) {
            game_menu_state(menu!=0);
            for (const Button &b:buttons) {
                assert(in_mode(b.ctl,g_s.menu,true));
                at(SDL_EVENT_FINGER_DOWN,b.ctl); assert(update().b==b.button);
                const Geo target=place(g_s.layout,b.ctl==C_HEAVY?C_LIGHT:C_HEAVY,(float)screen_w,(float)screen_h);
                event(SDL_EVENT_FINGER_MOTION,1,target.c.x,target.c.y); assert(update().b==b.button);
                finger(g_s,1)->t0=SDL_GetTicksNS()-MIN_PRESS-1;
                event(SDL_EVENT_FINGER_UP,1,target.c.x,target.c.y); assert(update().b==0);
            }
            for (int dir=0;dir<4;++dir) {
                const float x[]={.75f,-.75f,0,0},y[]={0,0,-.75f,.75f};
                const uint64_t expected[]={DPAD_RIGHT,DPAD_LEFT,DPAD_UP,DPAD_DOWN};
                assert(in_mode(C_HDPAD,g_s.menu,true));
                at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,x[dir],y[dir]); assert(update().b==expected[dir]);
                at(SDL_EVENT_FINGER_CANCELED,C_HDPAD,1,x[dir],y[dir]); assert(update().b==0);
            }
            assert(!in_mode(C_ATTACK,g_s.menu,true) && !in_mode(C_CORNER,g_s.menu,true) && !in_mode(C_RFIRE,g_s.menu,true));
            at(SDL_EVENT_FINGER_DOWN,C_HEAVY,1); at(SDL_EVENT_FINGER_DOWN,C_LIGHT,2);
            assert(update().b==(B(BTN_A)|B(BTN_X)));
            at(SDL_EVENT_FINGER_CANCELED,C_HEAVY,1); assert(update().b==B(BTN_X));
            at(SDL_EVENT_FINGER_CANCELED,C_LIGHT,2); assert(update().b==0);
            const Geo a=place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h),x=place(g_s.layout,C_LIGHT,(float)screen_w,(float)screen_h);
            event(SDL_EVENT_FINGER_DOWN,1,(a.c.x+x.c.x)*.5f,(a.c.y+x.c.y)*.5f);
            assert(update().b!=(B(BTN_A)|B(BTN_X)));
            event(SDL_EVENT_FINGER_CANCELED,1,(a.c.x+x.c.x)*.5f,(a.c.y+x.c.y)*.5f); assert(update().b==0);
        }
    }
    puts("PASS: enabled classic ABXY/LR/ZLZR/Plus/Minus/L3/R3 and D-pad stay direct across all 15 weapons and menus; dragging never changes the pressed button and only two fingers combine A+X");
}
static void neutral_sticks(const Input &v) {
    assert(v.lx==0 && v.ly==0 && v.rx==0 && v.ry==0 && v.dx==0 && v.dy==0);
}
static void classic_sticks() {
    for (const ImVec2 size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) {
        classic_fixture((int)size.x,(int)size.y); cfg.touch_floating=1; drawn=0;
        std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
        const int face[]={C_HEAVY,C_LIGHT,C_DODGE,C_SHEATHE};
        for (int a=0;a<4;++a) {
            const Geo ga=place(g_s.layout,face[a],(float)screen_w,(float)screen_h);
            int part=0; assert(hit(g_s.layout,ga.c.x,ga.c.y,(float)screen_w,(float)screen_h,&part,1.22f,false,false,true,false,UNIVERSAL_LAYOUT)==face[a] && part==0);
            for (int b=a+1;b<4;++b) {
                const Geo gb=place(g_s.layout,face[b],(float)screen_w,(float)screen_h);
                assert(hypotf(ga.c.x-gb.c.x,ga.c.y-gb.c.y)>ga.r+gb.r);
            }
        }
        for (int menu=0;menu<2;++menu) {
            game_menu_state(menu!=0);
            const Geo l=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h),r=place(g_s.layout,C_RSTICK,(float)screen_w,(float)screen_h);
            assert(!classic_control(C_RSTICK) && !in_mode(C_RSTICK,g_s.menu,true) && !in_editor(C_RSTICK,true,UNIVERSAL_LAYOUT));
            int part=0; assert(hit(g_s.layout,r.c.x,r.c.y,(float)screen_w,(float)screen_h,&part,1.22f,true,g_s.menu,true,false,UNIVERSAL_LAYOUT)!=C_RSTICK);
            assert(drawn_vertices(r.c,r.r*.7f)==0);
            event(SDL_EVENT_FINGER_DOWN,1,l.c.x+l.r*.25f,l.c.y);
            Input v=update(); assert(abs(v.lx-8191)<250 && v.ly==0 && !v.rx && !v.ry && !v.b && !g_s.run && !v.dx && !v.dy);
            event(SDL_EVENT_FINGER_MOTION,1,l.c.x+2*l.r,l.c.y);
            v=update(); assert(v.lx>32000 && !v.rx && !v.ry && !v.b && !g_s.run && !v.dx && !v.dy);
            event(SDL_EVENT_FINGER_DOWN,2,r.c.x,r.c.y); assert(finger(g_s,2) && finger(g_s,2)->kind==F_CAMERA);
            event(SDL_EVENT_FINGER_MOTION,2,r.c.x+.15f*screen_h,r.c.y-.08f*screen_h);
            v=update(); assert(v.lx>32000 && !v.rx && !v.ry && !g_s.run);
            if (menu) assert(v.b==DPAD_RIGHT && !v.dx && !v.dy);
            else assert(!v.b && v.dx>0 && v.dy<0);
            event(SDL_EVENT_FINGER_UP,1,l.c.x+2*l.r,l.c.y); v=update(); assert(!v.lx && !v.ly && !v.rx && !v.ry);
            event(SDL_EVENT_FINGER_CANCELED,2,r.c.x+.15f*screen_h,r.c.y-.08f*screen_h); touch_reset(); neutral_sticks(update());
            at(SDL_EVENT_FINGER_DOWN,C_STICK,1,.6f,0);
            assert(update().lx>15000); touch_reset(); neutral_sticks(update());
            at(SDL_EVENT_FINGER_DOWN,C_STICK,1,.6f,0);
            settings_open=true; neutral_sticks(update()); settings_open=false; neutral_sticks(update());
            at(SDL_EVENT_FINGER_DOWN,C_STICK,1,.6f,0);
            cfg.touch_controls=TOUCH_OFF; neutral_sticks(update()); cfg.touch_controls=TOUCH_ON; neutral_sticks(update());
            assert(update(B(BTN_R),true).b==B(BTN_R));
            Input physical; physical.b=B(BTN_R); physical.rx=12345; physical.ry=-23456;
            physical.touch_b=touch_input(true,false,&physical.b,&physical.lx,&physical.ly,&physical.rx,&physical.ry,&physical.dx,&physical.dy);
            assert(physical.b==B(BTN_R) && physical.touch_b==0 && physical.rx==12345 && physical.ry==-23456);
        }
        const Layout original=g_s.layout;
        for (int editing=0;editing<2;++editing) {
            if (editing) { touch_edit_begin(); update(); assert(g_s.edit); }
            const std::vector<ImVec2> before=touch_geometry();
            g_s.layout.x[C_RSTICK]=.681f; g_s.layout.y[C_RSTICK]=.541f; g_s.layout.scale[C_RSTICK]=1.83f;
            same_geometry(before,touch_geometry());
            g_s.layout=original;
            if (editing) { editor_click(E_CANCEL); update(); }
        }
    }
    puts("PASS: classic left stick keeps fixed linear axes at 16:9/21:9/32:9 and clears on reset/settings/Off; hidden right-stick coordinates never draw or hit in game/editor, swipe camera/menu gestures and physical right axes remain available");
}
static void classic_editor_roundtrip() {
    classic_fixture();
    g_s.layout.x[C_RSTICK]=.343f; g_s.layout.y[C_RSTICK]=.341f; g_s.layout.scale[C_RSTICK]=1.25f;
    assert(save_layout(g_s.layout,UNIVERSAL_LAYOUT)); g_s.saved=g_s.layout;
    touch_edit_begin(); update(); assert(g_s.edit);
    const Geo l=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
    event(SDL_EVENT_FINGER_DOWN,1,l.c.x,l.c.y); assert(g_s.sel==C_STICK);
    event(SDL_EVENT_FINGER_MOTION,1,l.c.x+70,l.c.y-30); event(SDL_EVENT_FINGER_UP,1,l.c.x+70,l.c.y-30);
    editor_click(E_BIGGER); const Layout edited=g_s.layout;
    assert(edited.scale[C_STICK]>1.09f && fabsf(edited.x[C_RSTICK]-.343f)<.0001f && fabsf(edited.y[C_RSTICK]-.341f)<.0001f && edited.scale[C_RSTICK]==1.25f);
    editor_click(E_STYLE); same_layout(g_s.layout,edited);
    editor_click(E_SAVE); update(); assert(!g_s.edit && g_s.layout_profile==UNIVERSAL_LAYOUT && profile==0);
    Layout saved; assert(load_layout(&saved,UNIVERSAL_LAYOUT)); same_layout(saved,edited);
    char path[40]; layout_path(UNIVERSAL_LAYOUT,path,sizeof path); std::ifstream f(path);
    const std::string text{std::istreambuf_iterator<char>(f),{}}; assert(text.find("stick ")!=std::string::npos && text.find("right_stick ")!=std::string::npos);
    touch_edit_begin(); update(); editor_click(E_RESET); same_layout(g_s.layout,defaults(UNIVERSAL_LAYOUT));
    editor_click(E_CANCEL); update(); same_layout(g_s.layout,edited);
    puts("PASS: classic left-stick drag/size saves while old hidden right-stick coordinates round-trip unchanged; universal binding/style stays fixed and Defaults/Cancel preserve the saved layout");
}
static void editor_control_art_clear(int c,const Rect &panel) {
    ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=ImVec2((float)screen_w,(float)screen_h); io.DeltaTime=1.0f/60;
    ImGui::NewFrame(); ImDrawList *dl=ImGui::GetForegroundDrawList();
    control(dl,ImGui::GetFont(),g_s.layout,c,(float)screen_w,(float)screen_h,g_s,1.0f);
    assert(dl->VtxBuffer.Size>0);
    for (const ImDrawVert &v:dl->VtxBuffer) assert(v.pos.y>panel.y1);
    ImGui::Render();
}
static void classic_editor_hit_targets() {
    for (const ImVec2 size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080),ImVec2(640,360)}) {
        fresh((int)size.x,(int)size.y); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
        for (int p=0;p<LAYOUT_SLOTS;++p) assert(save_layout(defaults(p),p));
        in_quest=false; update(); select_legacy_mode(g_s,1);
        const int entry=TOUCH_WEAPON_LAYOUTS+1;
        for (int p=0;p<LAYOUT_SLOTS;++p) {
            if (!available_layout(p)) continue;
            // Fixture setup loads a page directly; Next/autosave behavior is tested separately.
            touch_edit_begin(); update(); load_selected_layout(g_s,p); update(); assert(g_s.layout_profile==p);
            const Layout original=g_s.layout; std::vector<int> controls;
            for (int c=0;c<C_LAID;++c) if (in_editor(c,false,control_profile(g_s))) controls.push_back(c);
            if (p==UNIVERSAL_LAYOUT) {
                assert(std::find(controls.begin(),controls.end(),C_STICK)!=controls.end());
                assert(std::find(controls.begin(),controls.end(),C_RSTICK)==controls.end());
            }
            Rect panel,btn[E_COUNT]; editor_panel((float)screen_w,(float)screen_h,&panel,btn);
            assert(panel.y0==0 && fabsf(panel.y1-.06f*screen_h)<.01f);
            for (int b=0;b<E_COUNT;++b) {
                assert(fabsf(btn[b].y0-.005f*screen_h)<.01f && fabsf(btn[b].y1-.055f*screen_h)<.01f);
                assert(btn[b].x0>=panel.x0 && btn[b].x1<=panel.x1 && btn[b].x1>btn[b].x0);
            }
            editor_click(E_CANCEL); update();
            for (int c:controls) {
                touch_edit_begin(); update(); load_selected_layout(g_s,p); update(); assert(g_s.edit && g_s.layout_profile==p);
                same_layout(g_s.layout,original);
                const Geo before=place(g_s.layout,c,(float)screen_w,(float)screen_h);
                assert(before.c.y-before.r>panel.y1); // Check the entire default disk, not only its centre.
                editor_control_art_clear(c,panel);  // Check native outlines and icons as well as the disk bounds.
                ImVec2 point=before.c;
                if (c==C_HDPAD) point.x+=before.r*.6f;
                if (c==C_CORNER) point=band_point(before,before.r*BAND_LEN*.5f,0);
                for (int b=0;b<E_COUNT;++b) assert(!btn[b].in(point.x,point.y));
                assert(hit(g_s.layout,point.x,point.y,(float)screen_w,(float)screen_h,nullptr,1.5f,true,false,false,true,control_profile(g_s))==c);
                event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y);
                assert(g_s.edit && g_s.sel==c && finger(g_s,1) && finger(g_s,1)->kind==F_EDIT && finger(g_s,1)->ctl==c);
                const float dx=(from_right(g_s.layout,c)?-.025f:.025f)*screen_h,dy=-.018f*screen_h;
                event(SDL_EVENT_FINGER_MOTION,1,point.x+dx,point.y+dy);
                const Geo moved=place(g_s.layout,c,(float)screen_w,(float)screen_h);
                assert(fabsf(moved.c.x-before.c.x-dx)<.1f && fabsf(moved.c.y-before.c.y-dy)<.1f);
                event(SDL_EVENT_FINGER_UP,1,point.x+dx,point.y+dy); assert(g_s.edit && g_s.sel==c);
                editor_click(E_CANCEL); update(); assert(!g_s.edit && profile==entry && g_s.layout_profile==entry);
                Layout saved; assert(load_layout(&saved,p)); same_layout(saved,original);
            }
        }
    }
    puts("PASS: all default controls across all 22 accessible pages at 16:9/21:9/32:9/640x360 keep complete disks and native art below the top toolbar, remain selectable/draggable, and Cancel preserves each page; hidden right stick is absent");
}
static void classic_activation() {
    preset_fixture(); const int entry=TOUCH_WEAPON_LAYOUTS+1,lbg=profile_for_weapon(6),cat=profile_for_weapon(15),slot=RESERVED_LAYOUT_BASE;
    touch_edit_begin(); update(); editor_page(UNIVERSAL_LAYOUT);
    g_s.layout.x[C_RSTICK]=.343f; g_s.layout.y[C_RSTICK]=.341f; g_s.layout.scale[C_RSTICK]=1.25f;
    const Layout universal=g_s.layout; editor_click(E_STYLE); same_layout(g_s.layout,universal);
    editor_click(E_SAVE); update(); assert(!g_s.edit && profile==entry && g_s.layout_profile==entry);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(active_layout(g_s,p)==UNIVERSAL_LAYOUT);
    State restored{}; load_preset_selection(restored);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(active_layout(restored,p)==UNIVERSAL_LAYOUT);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) {
        in_quest=true; weapon_override=PROFILE_WEAPON[p]; ranged=ranged_profile(p); update();
        assert(g_s.layout_profile==UNIVERSAL_LAYOUT && profile==p && g_style==STYLE_CLASSIC && g_ranged==0);
    }
    in_quest=false; update(); assert(profile==entry && g_s.layout_profile==entry && g_s.legacy_mode==1);
    for (int step=1;step<=4;++step) {
        at(SDL_EVENT_FINGER_DOWN,C_LAYOUT); at(SDL_EVENT_FINGER_UP,C_LAYOUT); update();
        assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+(step%2?3:1));
    }
    assert(profile==entry && g_s.layout_profile==entry);
    touch_edit_begin(); update(); editor_page(lbg); editor_click(E_SAVE); update();
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) assert(active_layout(g_s,p)==(p==lbg?lbg:UNIVERSAL_LAYOUT));
    touch_edit_begin(); update(); editor_page(slot); editor_bind(cat);
    Layout inherited=universal; inherited.weapon_profile=cat; same_layout(g_s.layout,inherited);
    g_s.layout.x[C_RSTICK]=.394f; editor_click(E_SAVE); update();
    Layout saved; assert(load_layout(&saved,slot) && saved.style==STYLE_CLASSIC && saved.weapon_profile==cat && fabsf(saved.x[C_RSTICK]-.394f)<.0001f);
    State reloaded{}; load_preset_selection(reloaded);
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) {
        const int expected=p==lbg?lbg:p==cat?slot:UNIVERSAL_LAYOUT;
        assert(active_layout(g_s,p)==expected && active_layout(reloaded,p)==expected);
    }
    in_quest=true; weapon_override=15; update(); assert(g_s.layout_profile==slot && g_style==STYLE_CLASSIC && profile==cat && g_ranged==0);
    weapon_override=6; update(); assert(g_s.layout_profile==slot && profile==cat && g_style==STYLE_CLASSIC);
    weapon_override=0; update(); assert(g_s.layout_profile==slot && profile==cat && g_style==STYLE_CLASSIC);
    assert(touch_preset_use(lbg,-1)); assert(g_s.layout_profile==lbg && profile==lbg && g_style!=STYLE_CLASSIC);
    assert(touch_preset_use(TOUCH_UNIVERSAL_PRESET,-1)); assert(g_s.layout_profile==UNIVERSAL_LAYOUT && profile==0 && g_style==STYLE_CLASSIC);
    puts("PASS: Save universal/fixed/reserved selects a persistent quest source independently of real weapon changes, while legacy mappings and inherited classic style/right-stick placement remain intact");
}
static void classic_ranged_lock() {
    classic_fixture(); lock_on=true; hunt_camera=false; aiming=true; drawn=1;
    for (int p:{profile_for_weapon(6),profile_for_weapon(10)}) {
        weapon_override=PROFILE_WEAPON[p]; ranged=ranged_profile(p); update();
        for (int persistent=0;persistent<2;++persistent) {
            persistent_lock_on=persistent!=0; g_s.lock=true;
            g_s.hunt_ns=SDL_GetTicksNS()-11000*MS; g_s.cam_ns=SDL_GetTicksNS()-500*MS;
            assert(g_ranged==0 && g_style==STYLE_CLASSIC && !g_s.menu);
            touch_lock_buttons(0,false); assert(g_s.lock && touch_lock_on() && touch_lock_active());
        }
    }
    puts("PASS: classic controls keep real bow/bowgun scoped lock-on active without the hunt-camera callback, including after the nonpersistent timeout");
}
static void quest_attack_styles() {
    fresh();
    for (int p = 0; p < TOUCH_WEAPON_LAYOUTS; ++p) {
        Layout l = defaults(p);
        if (fixed_quest_attack_style(p)) {
            assert(l.style == STYLE_SEPARATE);
            l.style = STYLE_CORNER;  // An old file must not restore a removed attack style.
        } else {
            assert(l.style == STYLE_PILL);
            l.style = STYLE_CORNER;  // Ranged profiles keep their stored style for compatibility.
        }
        char path[40]; layout_path(p, path, sizeof path); save_layout(l, p);
        Layout loaded; assert(load_layout(&loaded, p));
        assert(loaded.style == (fixed_quest_attack_style(p) ? STYLE_SEPARATE : STYLE_CORNER));
        g_s.layout_profile = p; g_s.in_quest = true;
        assert(editor_style_locked(g_s));
    }
    g_s.layout_profile = 0;
    for (int p = 1; p < TOUCH_WEAPON_LAYOUTS; ++p) {
        Layout old = defaults(p); old.style = STYLE_PILL; save_layout(old, p);
        select_layout(g_s, p);
        assert(g_s.layout.style == (fixed_quest_attack_style(p) ? STYLE_SEPARATE : STYLE_PILL));
        assert(g_style == g_s.layout.style);
    }
    for (int weapon : {0, 15}) {
        select_layout(g_s, profile_for_weapon(weapon));
        touch_edit_begin(); update(); assert(g_s.edit);
        editor_click(E_STYLE); assert(g_s.layout.style == STYLE_SEPARATE && g_style == STYLE_SEPARATE);
        editor_click(E_RESET); assert(g_s.layout.style == STYLE_SEPARATE && g_style == STYLE_SEPARATE);
        editor_click(E_CANCEL); assert(!g_s.edit && g_s.layout.style == STYLE_SEPARATE);
        assert(in_mode(C_HEAVY, false, true) && in_mode(C_LIGHT, false, true));
        assert(!in_mode(C_ATTACK, false, true) && !in_mode(C_CORNER, false, true));
        at(SDL_EVENT_FINGER_DOWN, C_HEAVY); assert(update().b == B(BTN_A));
        at(SDL_EVENT_FINGER_CANCELED, C_HEAVY);
        at(SDL_EVENT_FINGER_DOWN, C_LIGHT); assert(update().b == B(BTN_X));
        at(SDL_EVENT_FINGER_CANCELED, C_LIGHT);
    }
    puts("PASS: quest melee and Prowler profiles keep Attacks: buttons and ignore old pill/corner files; ranged profiles stay compatible");
}
static void fixed_quest_menus() {
    for (const auto size : {ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) {
        for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) {
            fresh((int)size.x,(int)size.y);
            save_layout(defaults(p),p);
            weapon_override=PROFILE_WEAPON[p];
            ranged=weapon_override==4||weapon_override==6?WEAPON_BOWGUN:weapon_override==10?WEAPON_BOW:0;
            drawn=1; update(); assert(g_s.layout_profile==p);
            const Layout layout=g_s.layout;
            bool visible[C_COUNT]; Geo geometry[C_COUNT];
            for(int c=0;c<C_COUNT;++c) {
                visible[c]=in_mode(c,false,true);
                geometry[c]=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            }
            const int control=ranged?C_RDODGE:C_HEAVY;
            const uint64_t button=ranged?B(BTN_B):B(BTN_A);
            at(SDL_EVENT_FINGER_DOWN,control); assert(update().b==button);
            for(int step=0;step<6;++step) {
                game_menu_state(step%2==0);
                assert(update().b==button && !finger(g_s,1)->done && g_s.layout_profile==p);
                assert(!memcmp(&layout,&g_s.layout,sizeof layout));
                for(int c=0;c<C_COUNT;++c) {
                    const Geo after=place(g_s.layout,c,(float)screen_w,(float)screen_h);
                    assert(visible[c]==in_mode(c,g_s.menu,true));
                    assert(after.c.x==geometry[c].c.x && after.c.y==geometry[c].c.y && after.r==geometry[c].r);
                }
            }
            at(SDL_EVENT_FINGER_CANCELED,control); assert(!update().b);
            game_menu_state(true);
            cfg.touch_show[TB_DPAD]=1;
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,2,.75f,0); assert(update().b&B(BTN_RIGHT));
            at(SDL_EVENT_FINGER_CANCELED,C_HDPAD,2); assert(!update().b);
            touch_edit_begin(); update();
            editor_click(E_SET); assert(g_s.layout_profile==p && g_s.in_quest);
            assert(!in_editor(C_LAYOUT,true,p));
            editor_click(E_CANCEL);
        }
    }
    fresh();
    puts("PASS: all 15 quest profiles at 16:9/21:9/32:9 keep button visibility/geometry and held input through menu transitions; navigation works and editor cannot switch presets");
}
static int bow_drag_fixture(int type=0, bool classic=false, int w=1280, int h=720) {
    if(classic) classic_fixture(w,h); else fresh(w,h);
    local_hunter=0x08004000; ranged=WEAPON_BOW; drawn=1; bow_type=type;
    if(!classic) assert(save_layout(defaults(profile_for_weapon(10)),profile_for_weapon(10)));
    update(); bow_drag={};
    return classic ? (type ? C_R : C_LIGHT) : C_RFIRE;
}
static void bow_drag_counts(float x,float y) {
    float k=CAMERA_COUNTS/(float)screen_h;
    assert(fabsf(bow_drag.dx-x*k)<.01f && fabsf(bow_drag.dy-y*k)<.01f);
}
static void bow_charge_drag() {
    for(int type=0;type<2;++type) for(bool classic:{false,true}) for(int w:{1280,2520,3840}) {
        int control=bow_drag_fixture(type,classic,w,w==1280?720:1080);
        const uint64_t charge=B(type?BTN_R:BTN_X);
        Geo g=place(g_s.layout,control,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        assert(bow_drag.held && bow_drag.begins==1 && bow_drag.hunter==local_hunter && bow_drag.type==type);
        assert(update().b==charge && !bow_drag.adds);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+5,g.c.y-4);
        assert(!bow_drag.adds && !update().dx);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+30,g.c.y-20);
        bow_drag_counts(30,-20); // The first accepted drag includes the earlier subthreshold movement.
        Input held=update(); assert(held.b==charge && held.touch_b==charge && !held.dx && !held.dy);
        for(int i=0;i<3;++i) update(); // The native charge may start after touch ownership; no new samples appear.
        assert(bow_drag.adds==1);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+45,g.c.y-35); bow_drag_counts(45,-35);
        finger(g_s,1)->t0=SDL_GetTicksNS()-100*MS;
        event(SDL_EVENT_FINGER_UP,1,g.c.x+55,g.c.y-40); bow_drag_counts(55,-40);
        assert(!bow_drag.held && bow_drag.ends==1 && !bow_drag.cancels && !update().b);
        assert(!g_s.cam_dx && !g_s.cam_dy && g_s.bow_drag_finger<0);
        event(SDL_EVENT_FINGER_DOWN,2,g.c.x,g.c.y); assert(bow_drag.begins==2);
        bow_drag_counts(0,0); event(SDL_EVENT_FINGER_CANCELED,2,g.c.x+150,g.c.y);
        assert(!bow_drag.held && bow_drag.cancels==1); bow_drag_counts(0,0);
    }
    puts("PASS: drawn Bow Type 1/2 fire and classic X/R own a separate charge-drag channel; first threshold keeps DOWN displacement, UP keeps its last delta, cancel clears, and no aim key is synthesized");
}
static void bow_drag_sources() {
    int control=bow_drag_fixture(); Geo g=place(g_s.layout,control,(float)screen_w,(float)screen_h);
    event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
    event(SDL_EVENT_FINGER_DOWN,2,g.c.x,g.c.y);
    event(SDL_EVENT_FINGER_MOTION,2,g.c.x+100,g.c.y-80);
    assert(bow_drag.begins==1 && !bow_drag.adds && !update().dx);
    event(SDL_EVENT_FINGER_CANCELED,2,g.c.x+100,g.c.y-80); assert(bow_drag.held);
    event(SDL_EVENT_FINGER_DOWN,3,screen_w*.5f,screen_h*.3f); assert(finger(g_s,3)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_MOTION,3,screen_w*.5f+50,screen_h*.3f-25);
    Input camera=update(); assert(camera.dx>0 && camera.dy<0 && !bow_drag.adds);
    event(SDL_EVENT_FINGER_CANCELED,3,screen_w*.5f+50,screen_h*.3f-25);
    event(SDL_EVENT_FINGER_MOTION,1,g.c.x+35,g.c.y-25); bow_drag_counts(35,-25);
    event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+35,g.c.y-25);

    for(int type=0;type<2;++type) {
        bow_drag_fixture(type); at(SDL_EVENT_FINGER_DOWN,C_RAIM);
        at(SDL_EVENT_FINGER_MOTION,C_RAIM,1,2,-1);
        Input aim=update(); assert(aim.dx>0 && aim.dy<0 && aim.b==B(BTN_R) && !bow_drag.begins);
        at(SDL_EVENT_FINGER_CANCELED,C_RAIM,1,2,-1);
    }
    for(int mode=0;mode<5;++mode) {
        bow_drag_fixture();
        if(mode==0) local_hunter=0;
        else if(mode==1) drawn=0;
        else if(mode==2) { ranged=WEAPON_BOWGUN; weapon_override=4; g_s.selected_layout=profile_for_weapon(10); }
        else if(mode==3) { ranged=0; weapon_override=0; g_s.selected_layout=profile_for_weapon(10); }
        else { // An actual Bow using a bowgun preset's Fire sends A, not the charge button.
            assert(save_layout(defaults(profile_for_weapon(6)),profile_for_weapon(6)));
            g_s.selected_layout=profile_for_weapon(6); load_selected_layout(g_s,g_s.selected_layout);
        }
        update(); at(SDL_EVENT_FINGER_DOWN,C_RFIRE); at(SDL_EVENT_FINGER_MOTION,C_RFIRE,1,2,-1);
        assert(!bow_drag.begins && !bow_drag.adds && update().dx>0);
        at(SDL_EVENT_FINGER_CANCELED,C_RFIRE,1,2,-1);
    }
    bow_drag_fixture(); at(SDL_EVENT_FINGER_DOWN,C_RSPECIAL);
    at(SDL_EVENT_FINGER_MOTION,C_RSPECIAL,1,2,-1);
    assert(!bow_drag.begins && update().b==(B(BTN_X)|B(BTN_A)));
    at(SDL_EVENT_FINGER_CANCELED,C_RSPECIAL,1,2,-1);
    // Same X bit on another preset's ordinary attack is outside the dedicated/classic charge controls.
    assert(save_layout(defaults(0),0)); g_s.selected_layout=0; load_selected_layout(g_s,0); update();
    at(SDL_EVENT_FINGER_DOWN,C_LIGHT); at(SDL_EVENT_FINGER_MOTION,C_LIGHT,1,-2,-1);
    assert(!bow_drag.begins); at(SDL_EVENT_FINGER_CANCELED,C_LIGHT,1,-2,-1);
    puts("PASS: one owning charge finger stays isolated from free camera, RAIM, ordinary attacks and combos; actual weapon and emitted button override selected preset identity");
}
static void bow_drag_lifecycle() {
    for(int mode=0;mode<14;++mode) {
        bow_drag_fixture(); Geo g=place(g_s.layout,C_RFIRE,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+50,g.c.y-30); assert(bow_drag.held && bow_drag.adds==1);
        if(mode==0) { settings_open=true; event(SDL_EVENT_FINGER_MOTION,1,g.c.x+70,g.c.y-30); }
        else if(mode==1) game_menu_state(true);
        else if(mode==2) { cfg.touch_controls=TOUCH_OFF; update(); }
        else if(mode==3) update(0,false,true);
        else if(mode==4) touch_reset();
        else if(mode==5) load_selected_layout(g_s,g_s.layout_profile);
        else if(mode==6) { touch_edit_begin(); update(); }
        else if(mode==7) { in_quest=false; update(); }
        else if(mode==8) { local_hunter+=0x1000; update(); }
        else if(mode==9) { bow_type=1; update(); }
        else if(mode==10) { drawn=0; update(); }
        else if(mode==11) { ranged=WEAPON_BOWGUN; update(); }
        else if(mode==12) { cfg.touch_show[TB_FIRE]=0; update(); }
        else { finger(g_s,1)->done=true; update(); }
        assert(!bow_drag.held && bow_drag.resets && g_s.bow_drag_finger<0); bow_drag_counts(0,0);
        const int starts=bow_drag.begins;
        event(SDL_EVENT_FINGER_UP,1,g.c.x+180,g.c.y-90);
        assert(bow_drag.begins==starts); bow_drag_counts(0,0);
    }
    puts("PASS: charge drag and pending displacement clear on menus, editor, hidden controls, typing, background, preset, scene, hunter, type, drawn, actual weapon and control changes; an old UP cannot revive them");
}
static void bow_drag_manual_aim() {
    for(int type=0;type<2;++type) {
        bow_drag_fixture(type); Geo g=place(g_s.layout,C_RFIRE,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+40,g.c.y-20); assert(bow_drag.adds==1);
        aiming=true; update(); assert(!bow_drag.held); bow_drag_counts(0,0);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+70,g.c.y-50);
        Input aim=update(); assert(aim.dx>0 && aim.dy<0 && bow_drag.begins==1);
        aiming=false; update();
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+100,g.c.y-70);
        assert(bow_drag.begins==2 && bow_drag.held); bow_drag_counts(30,-20);
        Input charge=update(); assert(!charge.dx && !charge.dy && charge.b==B(type?BTN_R:BTN_X));
        event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+100,g.c.y-70);
        aiming=true; at(SDL_EVENT_FINGER_DOWN,C_RFIRE); at(SDL_EVENT_FINGER_MOTION,C_RFIRE,1,2,-1);
        assert(update().dx>0 && bow_drag.begins==2);
        at(SDL_EVENT_FINGER_CANCELED,C_RFIRE,1,2,-1);
    }
    puts("PASS: manual scope keeps original Fire camera dragging; entering aim cancels charge pending data and leaving it can re-own the same finger using only new displacement");
}
static void ranged_controls() {
    fresh(); ranged=WEAPON_BOWGUN; drawn=1; update();
    g_s.layout.style=STYLE_SEPARATE; g_style=STYLE_SEPARATE;
    ImVec2 seam; float size; assert(separate_seam(g_s.layout,(float)screen_w,(float)screen_h,&seam,&size));
    int part=0;
    assert(hit(g_s.layout,seam.x,seam.y,(float)screen_w,(float)screen_h,&part,1.22f,false,false,true,false,g_s.layout_profile)==C_RFIRE);
    lock_on=true; g_s.lock=true; touch_lock_buttons(0,false);
    at(SDL_EVENT_FINGER_DOWN,C_RFIRE); assert(update().b==B(BTN_A));
    at(SDL_EVENT_FINGER_CANCELED,C_RFIRE); assert(update().b==0);
    for (int reverse=0;reverse<2;++reverse) {
        int first=reverse?C_RSECOND:C_RFIRE,second=reverse?C_RFIRE:C_RSECOND;
        at(SDL_EVENT_FINGER_DOWN,first,1);
        at(SDL_EVENT_FINGER_DOWN,second,2);
        assert(update().b==(B(BTN_X)|B(BTN_A)));
        at(SDL_EVENT_FINGER_CANCELED,first,1); at(SDL_EVENT_FINGER_CANCELED,second,2);
        assert(update().b==0);
    }
    at(SDL_EVENT_FINGER_DOWN,C_RSPECIAL); assert(update().b==(B(BTN_X)|B(BTN_A)));
    at(SDL_EVENT_FINGER_CANCELED,C_RSPECIAL);
    at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(update().b==B(BTN_R));
    at(SDL_EVENT_FINGER_MOTION,C_RAIM,1,2,-1); Input drag=update();
    assert(drag.dx>0 && drag.dy<0 && drag.b==B(BTN_R));
    assert(update().dx==0 && update().dy==0);
    at(SDL_EVENT_FINGER_CANCELED,C_RAIM,1,2,-1); assert(update().b==0 && update().dx==0);
    at(SDL_EVENT_FINGER_DOWN,C_RAIM); at(SDL_EVENT_FINGER_UP,C_RAIM);
    assert(update().b==B(BTN_R));  // Native R tap opens/toggles the scope; held R owns shoulder aim.
    touch_reset();
    aiming=true; g_menu_ns=SDL_GetTicksNS(); g_press_ns=g_menu_ns.load(); g_window_open=false;
    update(); assert(!g_s.menu && in_mode(C_RFIRE,g_s.menu,true));
    aiming=false; update(); assert(g_s.menu);
    fresh(); ranged=WEAPON_BOWGUN; update();
    at(SDL_EVENT_FINGER_DOWN,C_RFIRE); assert(update().b==B(BTN_X));
    drawn=1; assert(update().b==0); at(SDL_EVENT_FINGER_UP,C_RFIRE); assert(update().b==0);
    at(SDL_EVENT_FINGER_DOWN,C_RFIRE); assert(update().b==B(BTN_A));
    at(SDL_EVENT_FINGER_CANCELED,C_RFIRE);
    fresh(); ranged=WEAPON_BOW; drawn=1; update();
    for (int type=0;type<2;++type) {
        bow_type=type; update();
        at(SDL_EVENT_FINGER_DOWN,C_RFIRE); assert(update().b==(type?B(BTN_R):B(BTN_X)));
        at(SDL_EVENT_FINGER_CANCELED,C_RFIRE);
        at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(update().b==B(BTN_R));
        at(SDL_EVENT_FINGER_MOTION,C_RAIM,1,2,-1); assert(update().dx>0);
        at(SDL_EVENT_FINGER_CANCELED,C_RAIM,1,2,-1); assert(update().b==0);
    }
    puts("PASS: fire stays A without forced scope/seam, native X+A in either order, aim tap/drag/release, scoped menu suppression, draw press cannot reload");
}
static void ranged_guard_migration() {
    fresh(); const int hbg=profile_for_weapon(4),lbg=profile_for_weapon(6),bow=profile_for_weapon(10);
    const int sources[]={hbg,lbg,bow,TOUCH_WEAPON_LAYOUTS+3,RESERVED_LAYOUT_BASE};
    for (int show=0;show<2;++show) {
        cfg.touch_show[TB_RANGED]=show;
        for (int slot:sources) {
            Layout old=defaults(reserved_layout(slot)?lbg:slot);
            old.x[C_R]=DEFS[C_R].x; old.y[C_R]=DEFS[C_R].y; old.scale[C_R]=1;
            old.style=STYLE_CORNER;
            assert(save_layout(old,slot)); Layout loaded; assert(load_layout(&loaded,slot));
            assert(loaded.x[C_R]==.58f && loaded.y[C_R]==.78f && loaded.scale[C_R]==1);
            const std::string original=stored_layout_text(slot);
            assert(load_layout(&loaded,slot) && stored_layout_text(slot)==original);
            for (int custom=0;custom<3;++custom) {
                Layout placed=old;
                if (custom==0) placed.x[C_R]=.317f;
                if (custom==1) placed.y[C_R]=.271f;
                if (custom==2) placed.scale[C_R]=1.17f;
                assert(save_layout(placed,slot) && load_layout(&loaded,slot));
                assert(loaded.x[C_R]==placed.x[C_R] && loaded.y[C_R]==placed.y[C_R] && loaded.scale[C_R]==placed.scale[C_R]);
            }
            Layout classic=old; classic.style=STYLE_CLASSIC;
            assert(save_layout(classic,slot) && load_layout(&loaded,slot));
            assert(loaded.style==STYLE_CLASSIC && loaded.x[C_R]==classic.x[C_R] && loaded.y[C_R]==classic.y[C_R]);
        }
    }
    for (int p=0;p<TOUCH_WEAPON_LAYOUTS;++p) {
        const Layout normal=defaults(p);
        assert(normal.x[C_R]==(ranged_profile(p)? .58f:DEFS[C_R].x));
        assert(normal.y[C_R]==(ranged_profile(p)? .78f:DEFS[C_R].y));
    }
    const Layout universal=defaults(UNIVERSAL_LAYOUT);
    assert(universal.style==STYLE_CLASSIC && universal.x[C_R]==.12f && universal.y[C_R]==.84f);
    puts("PASS: hidden ranged R's exact old default migrates for HBG/LBG/bow/village/reserved sources independently of ShowRanged; custom position/size and classic layouts are retained without rewriting files");
}
static void ranged_guard_input() {
    const int profiles[]={profile_for_weapon(4),profile_for_weapon(6),profile_for_weapon(10),TOUCH_WEAPON_LAYOUTS+3};
    for (const auto size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) {
        for (int p:profiles) {
            fresh((int)size.x,(int)size.y); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
            Layout saved=defaults(p);
            saved.x[C_R]=.613f; saved.y[C_R]=.817f; saved.scale[C_R]=1.123f;
            assert(save_layout(saved,p)); cfg.touch_show[TB_AIM]=0;
            if (p<TOUCH_WEAPON_LAYOUTS) { weapon_override=PROFILE_WEAPON[p]; ranged=ranged_profile(p); drawn=1; update(); }
            else { in_quest=false; update(); select_legacy_mode(g_s,3); update(); }
            assert(g_s.layout_profile==p && in_mode(C_RAIM,false,g_s.in_quest) && in_editor(C_RAIM,g_s.in_quest,p));
            assert(!in_mode(C_R,false,g_s.in_quest) && !in_editor(C_R,g_s.in_quest,p));
            const Geo guard=place(g_s.layout,C_RAIM,(float)screen_w,(float)screen_h);
            const Geo removed=place(g_s.layout,C_R,(float)screen_w,(float)screen_h);
            assert(hit(g_s.layout,guard.c.x,guard.c.y,(float)screen_w,(float)screen_h,nullptr,1.22f,false,false,g_s.in_quest,false,p)==C_RAIM);
            assert(hit(g_s.layout,removed.c.x,removed.c.y,(float)screen_w,(float)screen_h,nullptr,1.22f,false,false,g_s.in_quest,false,p)!=C_R);
            for(int state:{-1,0,1}) for(int type=0;type<2;++type) {
                drawn=state; bow_type=type; local_hunter=ranged==WEAPON_BOW?0x08004000:0; update();
                Icon icon; char letter[8]; ranged_face(C_RAIM,ranged_layout_profile(p),state,&icon,letter,sizeof letter);
                assert(icon==I_LETTER && !strcmp(letter,"R") && !strcmp(DEFS[C_RAIM].name,"R"));
                assert(ranged_buttons(C_RAIM,state,ranged_layout_profile(p))==B(BTN_R));
                at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(finger(g_s,1)->ctl==C_RAIM);
                for(int frame=0;frame<3;++frame) {
                    const Input held=update(); assert(held.b==B(BTN_R) && held.touch_b==B(BTN_R) && !held.dx && !held.dy);
                }
                assert(!bow_drag.begins); // R remains separate even when Bow Type 2 also charges with R.
                at(SDL_EVENT_FINGER_CANCELED,C_RAIM); assert(!update().b && !pulses(B(BTN_R)));
            }
            at(SDL_EVENT_FINGER_DOWN,C_RAIM); at(SDL_EVENT_FINGER_UP,C_RAIM);
            assert(update().touch_b==B(BTN_R) && pulses(B(BTN_R))==1); touch_reset(); assert(!update().b);
            for (int target:{C_RFIRE,C_RSECOND,C_AMMO_DOWN}) {
                at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(update().b==B(BTN_R));
                const Geo other=place(g_s.layout,target,(float)screen_w,(float)screen_h);
                event(SDL_EVENT_FINGER_MOTION,1,other.c.x,other.c.y);
                const Input moved=update(); assert(finger(g_s,1)->ctl==C_RAIM && moved.b==B(BTN_R) && (moved.dx || moved.dy));
                assert(!g_s.ammo_hold && !pulses(B(BTN_X)) && !pulses(B(BTN_B)));
                event(SDL_EVENT_FINGER_CANCELED,1,other.c.x,other.c.y); assert(!update().b);
            }
            for(int type=0;type<2;++type) for(int other:{C_RFIRE,C_RSECOND}) {
                bow_type=type; drawn=1; update();
                const uint64_t b=ranged_buttons(other,drawn,ranged_layout_profile(p));
                at(SDL_EVENT_FINGER_DOWN,C_RAIM,1); at(SDL_EVENT_FINGER_DOWN,other,2);
                assert(update().b==(B(BTN_R)|b));
                at(SDL_EVENT_FINGER_CANCELED,C_RAIM,1); assert(update().b==b);
                at(SDL_EVENT_FINGER_CANCELED,other,2); assert(!update().b);
            }
            assert(drawn_vertices(guard.c,guard.r*.5f)>20);
            at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(update().b==B(BTN_R));
            cfg.touch_show[TB_GUARD]=0; const Input disabled=update();
            assert(!disabled.b && !disabled.touch_b && !in_mode(C_RAIM,false,g_s.in_quest) && !in_editor(C_RAIM,g_s.in_quest,p));
            assert(update(B(BTN_R)).b==B(BTN_R)); // Hiding touch R never releases a physical or other touch source.
            cfg.touch_show[TB_AIM]=1; assert(!in_mode(C_RAIM,false,g_s.in_quest));
            assert(drawn_vertices(guard.c,guard.r*.5f)==0);
            assert(hit(g_s.layout,guard.c.x,guard.c.y,(float)screen_w,(float)screen_h,nullptr,1.22f,false,false,g_s.in_quest,false,p)!=C_RAIM);
            at(SDL_EVENT_FINGER_UP,C_RAIM); touch_reset();
            at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(finger(g_s,1)->kind!=F_BUTTON || finger(g_s,1)->ctl!=C_RAIM);
            assert(!update().b); at(SDL_EVENT_FINGER_CANCELED,C_RAIM);
            cfg.touch_show[TB_GUARD]=1; cfg.touch_show[TB_AIM]=0; assert(!update().b);
            at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(update().b==B(BTN_R)); at(SDL_EVENT_FINGER_CANCELED,C_RAIM);
            touch_edit_begin(); update(); at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(g_s.sel==C_RAIM);
            event(SDL_EVENT_FINGER_MOTION,1,guard.c.x-18,guard.c.y+22);
            event(SDL_EVENT_FINGER_UP,1,guard.c.x-18,guard.c.y+22);
            editor_click(E_BIGGER); assert(g_s.layout.scale[C_RAIM]>1.09f);
            editor_click(E_CANCEL); update();
            const Geo restored=place(g_s.layout,C_RAIM,(float)screen_w,(float)screen_h);
            assert(restored.c.x==guard.c.x && restored.c.y==guard.c.y && restored.r==guard.r);
            Layout kept; assert(load_layout(&kept,p));
            assert(kept.x[C_R]==saved.x[C_R] && kept.y[C_R]==saved.y[C_R] && kept.scale[C_R]==saved.scale[C_R]);
        }
    }
    puts("PASS: four ranged layouts have one fixed R for drawn/away/unknown and Bow Type 1/2; ShowR alone controls draw/hit/editor/held, dragging keeps camera control, native combinations survive, removed R coordinates remain saved");
}
static void near_rect(const Rect &a,const Rect &b,float dx=0,float dy=0) {
    assert(a.ok && b.ok);
    assert(fabsf(b.x0-a.x0-dx)<.02f && fabsf(b.x1-a.x1-dx)<.02f);
    assert(fabsf(b.y0-a.y0-dy)<.02f && fabsf(b.y1-a.y1-dy)<.02f);
}
static Rect editor_hud_rect(int zone) {
    return editor_zone_rect(g_s.layout,zone,(float)screen_w,(float)screen_h);
}
static ImVec2 editor_hud_pick(int zone) {
    const Rect r=editor_hud_rect(zone); assert(r.ok);
    Rect panel,buttons[E_COUNT]; editor_panel((float)screen_w,(float)screen_h,&panel,buttons);
    const float fractions[]={.5f,.25f,.75f,.1f,.9f,.02f,.98f};
    for (float fy:fractions) for (float fx:fractions) {
        const ImVec2 p(r.x0+(r.x1-r.x0)*fx,r.y0+(r.y1-r.y0)*fy);
        if (p.x<0 || p.x>screen_w || p.y<0 || p.y>screen_h || panel.in(p.x,p.y)) continue;
        if (hit(g_s.layout,p.x,p.y,(float)screen_w,(float)screen_h,nullptr,1.0f,true,false,g_s.in_quest,true,control_profile(g_s))>=0) continue;
        bool covered=false;
        for (int k=0;k<zone;++k) covered|=editor_hud_rect(k).in(p.x,p.y);
        if (!covered) return p;
    }
    assert(false && "HUD test needs an exposed handle point outside visible buttons"); return {};
}
static void drag_editor_hud(int zone,float dx,float dy) {
    const Rect before=editor_hud_rect(zone); const Layout layout=g_s.layout;
    const ImVec2 point=editor_hud_pick(zone); const float x=point.x,y=point.y;
    event(SDL_EVENT_FINGER_DOWN,1,x,y);
    assert(g_s.sel==ZONE_SEL+zone && finger(g_s,1)->kind==F_EDIT && finger(g_s,1)->ctl==ZONE_SEL+zone);
    event(SDL_EVENT_FINGER_MOTION,1,x+dx,y+dy);
    near_rect(before,editor_hud_rect(zone),dx,dy);  // The handle follows before any renderer feedback arrives.
    for (int k=0;k<TZ_COUNT;++k) {
        assert(g_touch_zone_dx[k]==g_s.layout.zx[k] && g_touch_zone_dy[k]==g_s.layout.zy[k]);
        if (k!=zone) assert(g_s.layout.zx[k]==layout.zx[k] && g_s.layout.zy[k]==layout.zy[k]);
    }
    for (int c=0;c<C_COUNT;++c) assert(g_s.layout.x[c]==layout.x[c] && g_s.layout.y[c]==layout.y[c] && g_s.layout.scale[c]==layout.scale[c]);
    event(SDL_EVENT_FINGER_UP,1,x+dx,y+dy); const Input input=update();
    assert(!input.b && !input.touch_b && !input.lx && !input.ly && !input.dx && !input.dy);
}
static void editor_hud_dragging() {
    for (const auto size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080),ImVec2(1280,800)}) {
        for (int moved=0;moved<2;++moved) {
            fresh((int)size.x,(int)size.y); cfg.touch_hud=moved;
            assert(save_layout(defaults(0),0)); update(); touch_edit_begin(); update();
            assert(g_s.edit);
            for (int k=0;k<TZ_COUNT;++k) {
                assert(!g_editor_zones[k].bounds.ok && !zone_rect(zones(),k,(float)screen_w,(float)screen_h).ok);
                drag_editor_hud(k,-24,32); drag_editor_hud(k,13,-11);
            }
            const Layout saved=g_s.layout; editor_click(E_SAVE); update(); assert(!g_s.edit);
            Layout stored; assert(load_layout(&stored,0)); same_layout(stored,saved);
            touch_edit_begin(); update(); drag_editor_hud(TZ_TARGET,-20,18);
            editor_click(E_CANCEL); update(); assert(!g_s.edit); same_layout(g_s.layout,saved);
            touch_reset(); g_s={};
            std::fill(std::begin(g_editor_zones),std::end(g_editor_zones),EditorZone{});
            update(); same_layout(g_s.layout,saved);
            for (int k=0;k<TZ_COUNT;++k) assert(g_touch_zone_dx[k]==g_s.layout.zx[k] && g_touch_zone_dy[k]==g_s.layout.zy[k]);
        }
    }
    puts("PASS: all three HUD handles drag immediately without render feedback, including repeated drags at 16:9/21:9/32:9/16:10 with MoveHUD on/off; buttons stay fixed and offsets survive Save/Cancel/restart");
}
static void editor_hud_feedback() {
    fresh(); cfg.touch_hud=1; update();
    g_s.layout.zx[TZ_ITEM]=60; g_s.layout.zy[TZ_ITEM]=30;
    const ImVec2 base[TZ_COUNT]={{600,500},{830,210},{1080,130}};
    for (int k=0;k<TZ_COUNT;++k) {
        const float ox=g_s.layout.zx[k]*squeeze(),oy=g_s.layout.zy[k]*squeeze_y();
        touch_hud_piece(k,base[k].x,base[k].y,base[k].x+ox,base[k].y+oy,ox,oy);
    }
    touch_hud_frame(); Rect remembered[TZ_COUNT];
    for (int k=0;k<TZ_COUNT;++k) { remembered[k]=editor_hud_rect(k); assert(g_editor_zones[k].bounds.ok); }
    g_s.layout.zx[TZ_ITEM]+=20; g_s.layout.zy[TZ_ITEM]-=15; publish_zones(g_s.layout);
    near_rect(remembered[TZ_ITEM],editor_hud_rect(TZ_ITEM),20,-15); remembered[TZ_ITEM]=editor_hud_rect(TZ_ITEM);
    touch_hud_piece(TZ_ITEM,base[TZ_ITEM].x,base[TZ_ITEM].y,base[TZ_ITEM].x+80,base[TZ_ITEM].y+15,80,15);
    touch_hud_frame();
    for (int k=0;k<TZ_COUNT;++k) near_rect(remembered[k],editor_hud_rect(k));
    assert(!zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h).ok);
    assert(!zone_rect(zones(),TZ_TARGET,(float)screen_w,(float)screen_h).ok);
    g_zones.ns=SDL_GetTicksNS()-stale_limit()-1;
    assert(!zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h).ok);
    touch_hud_frame();  // A wholly empty render frame also keeps all editor handles.
    for (int k=0;k<TZ_COUNT;++k) near_rect(remembered[k],editor_hud_rect(k));
    touch_edit_begin(); update(); for (int k=0;k<TZ_COUNT;++k) drag_editor_hud(k,19,21);
    editor_click(E_CANCEL); update();
    for (int k=0;k<TZ_COUNT;++k) {
        const Rect r=editor_hud_rect(k); const float x=(r.x0+r.x1)*.5f,y=(r.y0+r.y1)*.5f;
        event(SDL_EVENT_FINGER_DOWN,1,x,y); const Finger *f=finger(g_s,1);
        assert(f && f->kind!=F_ITEM && f->kind!=F_ARTS && f->kind!=F_TARGET);
        event(SDL_EVENT_FINGER_CANCELED,1,x,y); touch_reset();
    }
    // A new preset uses its own offset, never the manual offset already included in cached renderer positions.
    Layout other=defaults(profile_for_weapon(10)); other.zx[TZ_ITEM]=-35; other.zy[TZ_ITEM]=42;
    const Layout previous=g_s.layout; const Rect old=editor_hud_rect(TZ_ITEM);
    assert(save_layout(other,profile_for_weapon(10)) && touch_preset_use(profile_for_weapon(10),-1));
    assert(g_s.layout_profile==profile_for_weapon(10) && profile==profile_for_weapon(10));
    const Rect changed=editor_hud_rect(TZ_ITEM);
    near_rect(old,changed,other.zx[TZ_ITEM]-previous.zx[TZ_ITEM],other.zy[TZ_ITEM]-previous.zy[TZ_ITEM]);
    // Incompatible render modes/anchors/aspects use a fresh fallback rather than old-scene coordinates.
    for (int mode=0;mode<3;++mode) {
        if (mode==0) cfg.touch_hud=0;
        if (mode==1) { cfg.touch_hud=1; g_ui_anchor_aspect=21.0f/9.0f; }
        if (mode==2) { g_ui_anchor_aspect=16.0f/9.0f; g_render_aspect=32.0f/9.0f; }
        for (int k=0;k<TZ_COUNT;++k) {
            const Rect actual=editor_hud_rect(k); const EditorZone cache=g_editor_zones[k]; g_editor_zones[k]={};
            const Rect fallback=editor_hud_rect(k); g_editor_zones[k]=cache; near_rect(actual,fallback);
        }
    }
    puts("PASS: per-HUD editor caches subtract renderer-applied offsets, survive missing/empty/expired live frames, reapply another preset correctly and reject incompatible mode/anchor/aspect; ordinary gameplay never receives ghost HUD hit regions");
}
static void editor_hud_priority_and_multitouch() {
    fresh(); assert(save_layout(defaults(0),0)); update(); touch_edit_begin(); update();
    const Geo button=place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h);
    const float sx=squeeze(),sy=squeeze_y();
    const ImVec2 point(button.c.x-1.3f*button.r,button.c.y);
    assert(hit(g_s.layout,point.x,point.y,(float)screen_w,(float)screen_h,nullptr,1.0f,true,false,g_s.in_quest,true,control_profile(g_s))<0);
    assert(hit(g_s.layout,point.x,point.y,(float)screen_w,(float)screen_h,nullptr,1.5f,true,false,g_s.in_quest,true,control_profile(g_s))==C_HEAVY);
    touch_hud_piece(TZ_ITEM,600,500,point.x,point.y,0,0); touch_hud_frame();
    assert(editor_hud_rect(TZ_ITEM).in(button.c.x,button.c.y));
    event(SDL_EVENT_FINGER_DOWN,1,button.c.x,button.c.y); assert(g_s.sel==C_HEAVY && finger(g_s,1)->ctl==C_HEAVY);
    event(SDL_EVENT_FINGER_MOTION,1,button.c.x-12,button.c.y+15);
    const Geo moved=place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h);
    assert(fabsf(moved.c.x-button.c.x+12)<.02f && fabsf(moved.c.y-button.c.y-15)<.02f);
    assert(!g_s.layout.zx[TZ_ITEM] && !g_s.layout.zy[TZ_ITEM]);
    event(SDL_EVENT_FINGER_UP,1,button.c.x-12,button.c.y+15);
    editor_click(E_CANCEL); update(); touch_edit_begin(); update();
    const Layout before=g_s.layout;
    event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y);
    assert(g_s.sel==ZONE_SEL+TZ_ITEM && finger(g_s,1)->ctl==ZONE_SEL+TZ_ITEM);
    event(SDL_EVENT_FINGER_DOWN,2,point.x+100,point.y+50);
    assert(!g_s.pinch && finger(g_s,2)->kind==F_MENU);
    event(SDL_EVENT_FINGER_MOTION,2,point.x+140,point.y+90);
    assert(g_s.layout.zx[TZ_ITEM]==before.zx[TZ_ITEM] && g_s.layout.zy[TZ_ITEM]==before.zy[TZ_ITEM]);
    event(SDL_EVENT_FINGER_MOTION,1,point.x-32,point.y+24);
    assert(fabsf(g_s.layout.zx[TZ_ITEM]-before.zx[TZ_ITEM]+32/sx)<.01f);
    assert(fabsf(g_s.layout.zy[TZ_ITEM]-before.zy[TZ_ITEM]-24/sy)<.01f);
    for (int c=0;c<C_COUNT;++c) assert(g_s.layout.scale[c]==before.scale[c] && g_s.layout.x[c]==before.x[c] && g_s.layout.y[c]==before.y[c]);
    event(SDL_EVENT_FINGER_UP,2,point.x+140,point.y+90); event(SDL_EVENT_FINGER_UP,1,point.x-32,point.y+24);
    Rect panel,buttons[E_COUNT]; editor_panel((float)screen_w,(float)screen_h,&panel,buttons);
    const float x=(buttons[E_SMALLER].x0+buttons[E_SMALLER].x1)*.5f,y=(buttons[E_SMALLER].y0+buttons[E_SMALLER].y1)*.5f;
    touch_hud_piece(TZ_TARGET,1200,55,x,y,0,0); touch_hud_frame();
    event(SDL_EVENT_FINGER_DOWN,1,x,y); assert(finger(g_s,1)->kind==F_MENU);
    event(SDL_EVENT_FINGER_UP,1,x,y); editor_click(E_CANCEL); update();
    puts("PASS: visible button graphics stay draggable over HUD, HUD wins over expanded button hit slop, toolbar wins over HUD, and extra fingers cannot pinch a HUD or alter button arrays");
}
static void ammo_repeat() {
    fresh(); ranged=WEAPON_BOWGUN; drawn=1; update();
    for (int control : {C_AMMO_UP,C_AMMO_DOWN}) {
        uint64_t button=control==C_AMMO_UP?B(BTN_X):B(BTN_B);
        at(SDL_EVENT_FINGER_DOWN,control); assert(update().b==B(BTN_L));
        assert(pulses(button)==1);
        g_s.ammo_pulse={};
        g_s.ammo_ready=SDL_GetTicksNS()-1; g_s.ammo_hold=SDL_GetTicksNS()+300*MS;
        assert(update().b==B(BTN_L)); assert(pulses(button)==1);
        uint64_t ready=g_s.ammo_ready; update(); assert(g_s.ammo_ready==ready && pulses(button)==1);
        at(SDL_EVENT_FINGER_UP,control); g_s.ammo_ready=SDL_GetTicksNS()-1;
        update(); assert(pulses(button)==1);  // No additional steps once the finger lifts.
        touch_reset(); assert(update().b==0 && !g_s.ammo_hold && !g_s.ammo_ready);
    }
    fresh(); ranged=WEAPON_BOWGUN; drawn=1; update();
    Geo arrow=place(g_s.layout,C_AMMO_UP,(float)screen_w,(float)screen_h);
    event(SDL_EVENT_FINGER_DOWN,1,arrow.c.x,arrow.c.y);
    g_s.ammo_pulse.from=SDL_GetTicksNS()+2000*MS;
    g_s.ammo_pulse.until=g_s.ammo_pulse.from+80*MS;
    g_s.ammo_ready=g_s.ammo_pulse.from+170*MS;
    g_s.ammo_hold=g_s.ammo_pulse.from+320*MS;
    const uint64_t ready=g_s.ammo_ready;
    event(SDL_EVENT_FINGER_MOTION,1,arrow.c.x,arrow.c.y+120);
    assert(pulses(B(BTN_B))==1 && !pulses(B(BTN_X)) && finger(g_s,1)->moved);
    for(int i=0;i<100;++i) {
        event(SDL_EVENT_FINGER_MOTION,1,arrow.c.x,arrow.c.y+(i%2?120:-120));
        assert(pulses(B(BTN_X))+pulses(B(BTN_B))==1 && g_s.ammo_ready==ready);
    }
    for(const Pulse &p:g_s.pulse) assert(!p.until);  // rapid arrows use no generic pulse slots
    g_s.ammo_pulse={}; g_s.ammo_ready=0;
    update(); assert(!pulses(B(BTN_X)) && !pulses(B(BTN_B))); // a drag stops blind repeat in the old direction
    event(SDL_EVENT_FINGER_CANCELED,1,arrow.c.x,arrow.c.y);
    assert(!update().b && !g_s.ammo_hold);
    puts("PASS: stationary ammo arrows repeat, sliding reverses one bounded pending step, dragging stops old-direction repeat, lift stops queueing, cancel/background release touch-owned L and ammo");
}
static void item_gesture_lifecycle() {
    for(int mode=0;mode<7;++mode) for(int ammo=0;ammo<2;++ammo) {
        fresh(); ranged=WEAPON_BOWGUN; drawn=1; update();
        ImVec2 p=hud(TZ_ITEM,640,620);
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
        event(SDL_EVENT_FINGER_MOTION,1,p.x+(ammo?0:100),p.y-(ammo?100:0));
        assert(update().b==B(BTN_L));
        if(ammo) assert(pulses(B(BTN_X))==1);
        else assert(touch_item_pending==1);
        const size_t accepted=item_steps.size();
        pulse(g_s,B(BTN_A),2000*MS);  // cancelling item input must not cancel another touch control
        if(mode==0) {
            settings_open=true;
            event(SDL_EVENT_FINGER_MOTION,1,p.x+100,p.y-100); // opening settings cancels even before the next input sample
        } else if(mode==1) game_menu_state(true);
        else if(mode==2) cfg.touch_controls=TOUCH_OFF;
        else if(mode==3) update(0,false,true);
        else if(mode==4) touch_reset();
        else if(mode==5) load_selected_layout(g_s,g_s.layout_profile);
        else { touch_edit_begin(); update(); }
        Input result=update(B(BTN_L)|B(BTN_X)|B(BTN_B),false,mode==3);
        assert((result.b&(B(BTN_L)|B(BTN_X)|B(BTN_B)))==(B(BTN_L)|B(BTN_X)|B(BTN_B)));
        assert(!(result.touch_b&(B(BTN_L)|B(BTN_X)|B(BTN_B))));
        assert(!pulses(B(BTN_X)) && !pulses(B(BTN_B)) && !g_s.ammo_hold && !g_s.ammo_ready && !g_s.bar_since);
        assert(!touch_item_pending);
        assert(!finger(g_s,1));
        if(mode==0 || mode==1 || mode==6) assert(pulses(B(BTN_A))==1);
        event(SDL_EVENT_FINGER_UP,1,p.x+100,p.y-200);
        assert(item_steps.size()==accepted && !pulses(B(BTN_Y)));
    }
    fresh(); cfg.touch_show[TB_ITEM_ARROWS]=1;
    ImVec2 p=hud(TZ_ITEM,640,400);
    Rect item=zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h);
    Geo arrow=item_arrow(item,1,(float)screen_h);
    event(SDL_EVENT_FINGER_DOWN,1,arrow.c.x,arrow.c.y);
    assert(finger(g_s,1)->kind==F_ITEM && item_steps==std::vector<int>{1});
    finger(g_s,1)->step_at=0;
    event(SDL_EVENT_FINGER_MOTION,1,arrow.c.x-100,arrow.c.y);
    assert(item_steps==(std::vector<int>{1,-1}));
    finger(g_s,1)->step_at=0;
    event(SDL_EVENT_FINGER_MOTION,1,arrow.c.x+100,arrow.c.y);
    assert(item_steps==(std::vector<int>{1,-1,1}));
    event(SDL_EVENT_FINGER_UP,1,arrow.c.x+100,arrow.c.y); assert(!update().b && touch_item_pending==1);
    event(SDL_EVENT_FINGER_DOWN,2,p.x,p.y);
    assert(!touch_item_pending);
    event(SDL_EVENT_FINGER_MOTION,2,p.x+10000,p.y);
    const size_t count=item_steps.size();
    for(int i=0;i<100;++i) update();
    assert(item_steps.size()<=count+1); // no long selection tail from an oversized flick
    event(SDL_EVENT_FINGER_CANCELED,2,p.x+10000,p.y); assert(!update().b && !touch_item_pending);
    puts("PASS: item/ammo gestures cancel on settings, game menus, hidden controls, typing, background, preset and editor; physical buttons survive, item arrows slide and large flicks leave no action queue");
}
static void target_switch(int w,int h) {
    fresh(w,h); lock_on=true; ImVec2 p=hud(TZ_TARGET,950,55);
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_TARGET);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); assert(g_s.lock && target_cycles==0);
    for (int i=0;i<3;++i) {
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
        assert(g_s.lock && target_cycles==i+1);
    }
    touch_lock_buttons(0,false); assert(touch_lock_on());
    touch_lock_toggle(); touch_lock_buttons(0,false); assert(!touch_lock_on());
    lock_on=false; event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
    assert(update().b==B(BTN_L) && target_cycles==3);
    puts("PASS: target icon enables one lock, later taps request next monster, controller toggles off, original camera tap preserved");
}
static void persistent_target_lock() {
    fresh(); lock_on = persistent_lock_on = true; g_s.lock = true;
    hunt_camera = false; g_s.hunt_ns = SDL_GetTicksNS() - 11000 * MS;
    touch_lock_buttons(0, false);
    assert(g_s.lock && !touch_lock_on() && !touch_lock_active());
    hunt_camera = true; g_s.cam_ns = SDL_GetTicksNS() - 500 * MS;
    touch_lock_buttons(0, false); assert(touch_lock_on() && touch_lock_active());
    settings_open = true; touch_lock_buttons(0, false);
    assert(g_s.lock && !touch_lock_on() && !touch_lock_active());
    settings_open = false; touch_lock_buttons(0, false); assert(touch_lock_on());
    game_menu_state(true); touch_lock_buttons(0, false); assert(g_s.lock && !touch_lock_on());
    game_menu_state(false); touch_lock_buttons(0, false); assert(touch_lock_on());
    g_s.edit = true; touch_lock_buttons(0, false); assert(g_s.lock && !touch_lock_on());
    g_s.edit = false; touch_reset(); touch_lock_buttons(0, false); assert(touch_lock_on());
    touch_lock_buttons(0, true); assert(g_s.lock && touch_lock_on() && !touch_lock_active());
    g_s.cam_ns = SDL_GetTicksNS() - 500 * MS;
    touch_lock_buttons(0, false); assert(touch_lock_active());
    in_quest = false; hunt_camera = false; update();
    g_s.hunt_ns = SDL_GetTicksNS() - 11000 * MS;
    touch_lock_buttons(0, false); assert(g_s.lock && !touch_lock_on());
    persistent_lock_on = false; touch_lock_buttons(0, false);
    assert(!g_s.lock && !touch_lock_on());

    fresh(); lock_on = persistent_lock_on = true; ImVec2 p = hud(TZ_TARGET, 950, 55);
    event(SDL_EVENT_FINGER_DOWN, 1, p.x, p.y);
    finger(g_s, 1)->t0 = SDL_GetTicksNS() - 600 * MS;
    event(SDL_EVENT_FINGER_UP, 1, p.x, p.y);
    assert(g_s.lock && target_cycles == 0);
    event(SDL_EVENT_FINGER_DOWN, 1, p.x, p.y); event(SDL_EVENT_FINGER_UP, 1, p.x, p.y);
    assert(g_s.lock && target_cycles == 1);
    event(SDL_EVENT_FINGER_DOWN, 1, p.x, p.y);
    finger(g_s, 1)->t0 = SDL_GetTicksNS() - 600 * MS;
    event(SDL_EVENT_FINGER_UP, 1, p.x, p.y);
    touch_lock_buttons(0, false); assert(!g_s.lock && !touch_lock_on() && target_cycles == 1);
    event(SDL_EVENT_FINGER_DOWN, 1, p.x, p.y);
    finger(g_s, 1)->t0 = SDL_GetTicksNS() - 600 * MS;
    event(SDL_EVENT_FINGER_CANCELED, 1, p.x, p.y);
    assert(!g_s.lock && target_cycles == 1);
    touch_lock_toggle(); touch_lock_buttons(0, false); assert(touch_lock_on());
    touch_lock_toggle(); touch_lock_buttons(0, false); assert(!touch_lock_on());
    puts("PASS: persistent lock survives camera timeout, menus, editor, background and quest changes; long target press toggles once, taps cycle and cancellation does nothing");
}
static void show_settings() {
    fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
    struct Pair { int setting, hunt, menu; };
    const Pair pairs[]={{TB_ATTACKS,C_HEAVY,C_HEAVY},{TB_DODGE,C_DODGE,C_DODGE},{TB_GUARD,C_R,C_R},
        {TB_SHEATHE,C_SHEATHE,C_SHEATHE},{TB_PLUS,C_MENU,C_MENU},{TB_MINUS,C_HMINUS,C_HMINUS},
        {TB_DPAD,C_HDPAD,C_HDPAD},{TB_L,C_L,C_L},{TB_ZL,C_ZL,C_ZL},{TB_ZR,C_ZR,C_ZR},
        {TB_L3,C_MAP,C_MAP},{TB_R3,C_R3,C_R3},{TB_STICK,C_STICK,C_STICK}};
    for (const Pair &p:pairs) {
        for (int menu=0;menu<=1;++menu) {
            const int c=menu?p.menu:p.hunt; if(c<0)continue;
            cfg.touch_show[p.setting]=1; assert(in_mode(c,menu!=0,true));
            cfg.touch_show[p.setting]=0; assert(!in_mode(c,menu!=0,true));
            cfg.touch_show[p.setting]=1;
        }
    }
    for (int c : {C_DPAD,C_MPILL,C_ML,C_MR,C_MZL,C_MZR,C_MINUS,C_FY})
        assert(!in_mode(c,true,true));
    g_town_ns=SDL_GetTicksNS(); cfg.touch_show[TB_ZL]=0; assert(!in_mode(C_ZL,false,true));
    g_style=STYLE_CORNER; cfg.touch_show[TB_GUARD]=0;
    assert(!in_mode(C_CORNER,false,true)&&!in_mode(C_R,false,true)&&in_mode(C_ATTACK,false,true));
    cfg.touch_show[TB_GUARD]=1; cfg.touch_show[TB_ATTACKS]=0;
    assert(!in_mode(C_CORNER,false,true)&&!in_mode(C_ATTACK,false,true)&&in_mode(C_R,false,true));
    cfg.touch_show[TB_ATTACKS]=1; g_style=STYLE_PILL; ranged=WEAPON_BOWGUN; update();
    const Pair guns[]={{TB_FIRE,C_RFIRE,-1},{TB_SECOND,C_RSECOND,-1},{TB_GUARD,C_RAIM,-1},
        {TB_RSPECIAL,C_RSPECIAL,-1},{TB_AMMO,C_AMMO_UP,-1},{TB_AMMO,C_AMMO_DOWN,-1},
        {TB_DODGE,C_RDODGE,-1},{TB_SHEATHE,C_RSHEATHE,-1}};
    for(const Pair &p:guns){cfg.touch_show[p.setting]=1;assert(in_mode(p.hunt,false,true));
        cfg.touch_show[p.setting]=0;assert(!in_mode(p.hunt,false,true));cfg.touch_show[p.setting]=1;}
    cfg.touch_show[TB_RANGED]=0; update(); assert(!in_mode(C_RFIRE,false,true)&&in_mode(C_ATTACK,false,true));
    cfg.touch_show[TB_RANGED]=1;assert(ranged_layout_profile(TOUCH_LAYOUT_PROFILES-1)==WEAPON_BOWGUN);
    cfg.touch_show[TB_RANGED]=0;assert(ranged_layout_profile(TOUCH_LAYOUT_PROFILES-1)==0);
    fresh(); cfg.touch_show[TB_ATTACKS]=0; g_style=STYLE_SEPARATE;
    ImVec2 seam;float size;assert(separate_seam(g_s.layout,(float)screen_w,(float)screen_h,&seam,&size));
    int part=0;assert(hit(g_s.layout,seam.x,seam.y,(float)screen_w,(float)screen_h,&part,1.22f,false,false,true,false,0)!=C_LIGHT);
    fresh();std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
    Geo stick=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
    for(int floating=0;floating<2;++floating) {
        cfg.touch_floating=floating;cfg.touch_show[TB_STICK]=1;
        assert(floating ? drawn_vertices(stick.c,stick.r*.75f)==0 : drawn_vertices(stick.c,stick.r*.75f)>20);
        at(SDL_EVENT_FINGER_DOWN,C_STICK);assert(finger(g_s,1)->kind==F_STICK);
        assert(drawn_vertices(stick.c,stick.r*.75f)>20);
        cfg.touch_show[TB_STICK]=0;assert(update().lx==0 && update().ly==0 && !g_s.stick_on);
        assert(drawn_vertices(stick.c,stick.r*.75f)==0);
        at(SDL_EVENT_FINGER_CANCELED,C_STICK);
    }
    // Drawing and hit handling must follow the same ShowItemArrows setting.
    fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
    hud(TZ_ITEM,640,400); Rect item=zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h);
    Geo arrow=item_arrow(item,1,(float)screen_h);
    assert(drawn_vertices(arrow.c,arrow.r)==0);
    event(SDL_EVENT_FINGER_DOWN,1,arrow.c.x,arrow.c.y); event(SDL_EVENT_FINGER_CANCELED,1,arrow.c.x,arrow.c.y);
    assert(item_steps.empty()); cfg.touch_show[TB_ITEM_ARROWS]=1;
    assert(drawn_vertices(arrow.c,arrow.r)>20);
    event(SDL_EVENT_FINGER_DOWN,2,arrow.c.x,arrow.c.y); event(SDL_EVENT_FINGER_UP,2,arrow.c.x,arrow.c.y);
    assert(item_steps==std::vector<int>{1});
    puts("PASS: every Show* binding across menu/melee/ranged, town ZL, corner guard, item arrows draw and hit");
}
static void preview(const char *path) {
    SDL_Surface *surface=SDL_CreateSurface(screen_w,screen_h,SDL_PIXELFORMAT_RGBA32);assert(surface);
    SDL_Renderer *renderer=SDL_CreateSoftwareRenderer(surface);assert(renderer);
    ImGui::CreateContext();ImGuiIO &io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize=ImVec2((float)screen_w,(float)screen_h);io.DeltaTime=1.0f/60;
    io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures|ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefaultVector();
    if (cfg.language==2) {
        std::string font=__FILE__; size_t slash=font.find_last_of("/\\"); assert(slash!=std::string::npos); font.resize(slash);
        slash=font.find_last_of("/\\"); assert(slash!=std::string::npos); font.resize(slash);
        font+="/android/app/src/main/assets/cjk.ttf";
        io.FontDefault=io.Fonts->AddFontFromFileTTF(font.c_str(),20); assert(io.FontDefault);
    }
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(14,14),IM_COL32(205,220,235,255),
        "HOST PREVIEW | Touch controls | No game scene / no Android device");
    touch_draw((float)screen_w,(float)screen_h);ImGui::Render();
    ImDrawData *data=ImGui::GetDrawData();assert(data&&data->TotalVtxCount>100);
    std::vector<ImTextureData *> textures;
    if(data->Textures)for(ImTextureData *tex:*data->Textures){
        if(tex->Status==ImTextureStatus_WantCreate){
            assert(tex->Format==ImTextureFormat_RGBA32);
            SDL_Texture *texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,tex->Width,tex->Height);assert(texture);
            assert(SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND));assert(SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_LINEAR));
            assert(SDL_UpdateTexture(texture,nullptr,tex->GetPixels(),tex->GetPitch()));
            tex->SetTexID((ImTextureID)(uintptr_t)texture);tex->SetStatus(ImTextureStatus_OK);textures.push_back(tex);
        }else if(tex->Status==ImTextureStatus_WantUpdates){
            assert(SDL_UpdateTexture((SDL_Texture *)(uintptr_t)tex->TexID,nullptr,tex->GetPixels(),tex->GetPitch()));
            tex->SetStatus(ImTextureStatus_OK);
        }
    }
    assert(SDL_SetRenderDrawColor(renderer,29,43,56,255));assert(SDL_RenderClear(renderer));
    for(const ImDrawList *list:data->CmdLists){
        std::vector<SDL_Vertex> vertices;vertices.reserve(list->VtxBuffer.Size);
        for(const ImDrawVert &v:list->VtxBuffer)vertices.push_back({{v.pos.x,v.pos.y},
            {((v.col>>IM_COL32_R_SHIFT)&255)/255.f,((v.col>>IM_COL32_G_SHIFT)&255)/255.f,
             ((v.col>>IM_COL32_B_SHIFT)&255)/255.f,((v.col>>IM_COL32_A_SHIFT)&255)/255.f},{v.uv.x,v.uv.y}});
        for(const ImDrawCmd &cmd:list->CmdBuffer){
            assert(!cmd.UserCallback);SDL_Rect clip{(int)cmd.ClipRect.x,(int)cmd.ClipRect.y,
                (int)(cmd.ClipRect.z-cmd.ClipRect.x),(int)(cmd.ClipRect.w-cmd.ClipRect.y)};
            assert(SDL_SetRenderClipRect(renderer,&clip));std::vector<int> indices(cmd.ElemCount);
            for(unsigned i=0;i<cmd.ElemCount;++i)indices[i]=list->IdxBuffer[cmd.IdxOffset+i]+cmd.VtxOffset;
            assert(SDL_RenderGeometry(renderer,(SDL_Texture *)(uintptr_t)cmd.GetTexID(),vertices.data(),(int)vertices.size(),indices.data(),(int)indices.size()));
        }
    }
    assert(SDL_FlushRenderer(renderer));assert(SDL_SavePNG(surface,path));
    for(ImTextureData *tex:textures){SDL_DestroyTexture((SDL_Texture *)(uintptr_t)tex->TexID);tex->SetTexID(ImTextureID_Invalid);tex->SetStatus(ImTextureStatus_Destroyed);}
    ImGui::DestroyContext();SDL_DestroyRenderer(renderer);SDL_DestroySurface(surface);
    printf("HOST PREVIEW: %s (%dx%d, actual ImGui draw lists / SDL software renderer; no game scene)\n",path,screen_w,screen_h);
}
int main() {
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS));
    window=SDL_CreateWindow("MHGU touch host regression",screen_w,screen_h,SDL_WINDOW_HIDDEN);assert(window);
    ImGui::CreateContext(); ImGuiIO &io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures;io.Fonts->AddFontDefaultVector();
    touch_button_sources();item_gestures();auto_r();floating_stick();dpad_visibility(1280,720);dpad_visibility(640,360);layout_roundtrip();show_settings();profile_layouts();quest_attack_styles();
    legacy_startup_compatibility();nonquest_layouts();nonquest_editor_profiles();editor_combo_isolation();editor_scene_transitions();
    reserved_preset_slots();reserved_activation_rebinding();reserved_cancel_defaults();preset_mapping_reload();reserved_quest_combos();reserved_scene_autosave();reserved_binding_drafts();editor_save_failure();
    direct_preset_selection();direct_preset_reapply();direct_preset_failure();directed_preset_editor();
    manual_preset_source_controls();manual_preset_persistence();manual_preset_editor_selection();
    classic_shortcut_compatibility();classic_show_settings();classic_dpad_shortcut();
    classic_controls();classic_sticks();classic_editor_roundtrip();classic_editor_hit_targets();classic_activation();classic_ranged_lock();
    fixed_quest_menus();ranged_controls();ranged_guard_migration();ranged_guard_input();
    bow_charge_drag();bow_drag_sources();bow_drag_lifecycle();bow_drag_manual_aim();
    editor_hud_dragging();editor_hud_feedback();editor_hud_priority_and_multitouch();
    ammo_repeat();item_gesture_lifecycle();target_switch(1280,720);target_switch(2520,1080);target_switch(3840,1080);persistent_target_lock();
    ImGui::DestroyContext();
    fresh();cfg.touch_show[TB_DPAD]=0;preview("touch-host-dpad-hidden-1280x720.png");
    cfg.touch_show[TB_DPAD]=1;update();preview("touch-host-dpad-shown-1280x720.png");
    touch_edit_begin();update();preview("touch-host-editor-1280x720.png");
    fresh(640,360);cfg.touch_show[TB_DPAD]=1;update();preview("touch-host-dpad-shown-640x360.png");
    touch_edit_begin();update();preview("touch-host-editor-640x360.png");
    fresh();in_quest=false;update();select_legacy_mode(g_s,1);touch_edit_begin();update();
    editor_page(profile_for_weapon(10));preview("touch-host-editor-bow-nonquest-1280x720.png");
    editor_page(profile_for_weapon(15));preview("touch-host-editor-prowler-nonquest-1280x720.png");
    editor_click(E_CANCEL);update();preview("touch-host-generic-after-editor-1280x720.png");
    fresh(640,360);in_quest=false;update();assert(save_layout(defaults(RESERVED_LAYOUT_BASE),RESERVED_LAYOUT_BASE));
    touch_edit_begin();update();editor_page(RESERVED_LAYOUT_BASE);preview("touch-host-editor-reserved-unbound-640x360.png");
    editor_bind(profile_for_weapon(6));preview("touch-host-editor-reserved-lbg-640x360.png");
    editor_click(E_CANCEL);update();
    classic_fixture();preview("touch-host-classic-1280x720.png");
    touch_edit_begin();update();preview("touch-host-editor-classic-1280x720.png");editor_click(E_CANCEL);update();
    classic_fixture(2520,1080);touch_edit_begin();update();preview("touch-host-editor-classic-2520x1080.png");editor_click(E_CANCEL);update();
    classic_fixture(3840,1080);preview("touch-host-classic-3840x1080.png");
    touch_edit_begin();update();preview("touch-host-editor-classic-3840x1080.png");editor_click(E_CANCEL);update();
    classic_fixture();std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);update();preview("touch-host-classic-all-shown-1280x720.png");
    cfg.touch_show[TB_DPAD]=0;update();preview("touch-host-classic-dpad-hidden-1280x720.png");
    cfg.touch_show[TB_DPAD]=1;update();preview("touch-host-classic-dpad-shown-1280x720.png");
    std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);update();preview("touch-host-classic-all-hidden-1280x720.png");
    classic_fixture(640,360);std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);cfg.language=2;touch_edit_begin();update();preview("touch-host-editor-classic-zh-640x360.png");
    editor_click(E_CANCEL);update();
    fresh(2520,1080); cfg.touch_hud=1; update(); touch_edit_begin(); update();
    drag_editor_hud(TZ_ITEM,-70,-80); drag_editor_hud(TZ_ARTS,-60,110); drag_editor_hud(TZ_TARGET,-80,130);
    preview("touch-host-editor-hud-drag-2520x1080.png"); editor_click(E_CANCEL); update();
    fresh(3840,1080); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
    assert(save_layout(defaults(profile_for_weapon(6)),profile_for_weapon(6)));
    weapon_override=6; ranged=WEAPON_BOWGUN; drawn=1; cfg.touch_hud=1; update();
    preview("touch-host-ranged-native-r-3840x1080.png");
    SDL_DestroyWindow(window);SDL_Quit();
    puts("PASS: all touch control regressions (host stubs, no APK/game execution)");return 0;
}
