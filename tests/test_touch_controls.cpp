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
static bool in_quest = true;
static bool aiming, lock_on, persistent_lock_on, hunt_camera = true;
static int target_cycles;
static std::vector<int> item_steps;
static int screen_w = 1280, screen_h = 720;
extern "C" {
volatile uint64_t g_town_ns;
float g_render_aspect = 16.0f / 9.0f;
uint64_t plat_now_ns(void) { return SDL_GetTicksNS(); }
SDL_Window *ui_window(void) { return window; }
uint32_t weapon_hunter(void) { return 0; }
int weapon_drawn(void) { return drawn; }
int weapon_ranged(uint32_t) { return ranged; }
int weapon_type(void) { return weapon_override >= 0 ? weapon_override : ranged == WEAPON_BOWGUN ? 4 : ranged == WEAPON_BOW ? 10 : 0; }
int presence_in_quest(void) { return in_quest; }
bool weapon_aiming(uint32_t) { return aiming; }
bool weapon_moving(uint32_t) { return true; }
unsigned weapon_action(uint32_t) { return 0; }
int weapon_bow_type(void) { return bow_type; }
bool mousecam_active(void) { return hunt_camera; }
void mousecam_target_cycle(void) { ++target_cycles; }
void itembar_scroll(int dir) { item_steps.push_back(dir); }
void haptics_tap(void) {}
bool overlay_menu_open(void) { return settings_open; }
void overlay_toggle_settings(void) { settings_open = !settings_open; }
bool overlay_event(const SDL_Event *) { return true; }
Config config_editable(void) { return cfg; }
int config_language(void) { return 0; }
int config_live_touch_controls(void) { return cfg.touch_controls; }
int config_live_touch_hud(void) { return 0; }
int config_live_touch_auto_r(void) { return cfg.touch_auto_r; }
int config_live_touch_floating(void) { return cfg.touch_floating; }
int config_live_touch_show(int b) { assert(b >= 0 && b < TB_COUNT); return cfg.touch_show[b]; }
void config_toggle_touch_dpad(void) { cfg.touch_show[TB_DPAD] = !cfg.touch_show[TB_DPAD]; }
int config_live_lock_on(void) { return lock_on; }
int config_live_persistent_lock_on(void) { return persistent_lock_on; }
uint32_t config_live_touch_combo(int, int) { return 0; }
int config_live_touch_profile(void) { return profile; }
void config_set_touch_profile(int value) { profile = value; }
float config_live_touch_camera(void) { return 1; }
float config_live_touch_opacity(void) { return .8f; }
void config_combo_text(uint32_t, char *out, size_t n) { if (n) *out = 0; }
}
struct Input { uint64_t b{}, touch_b{}; int lx{}, ly{}, rx{}, ry{}; float dx{}, dy{}; };
static Input update(uint64_t controller = 0, bool pad = false, bool typing = false) {
    Input v; v.b = controller;
    v.touch_b = touch_input(pad, typing, &v.b, &v.lx, &v.ly, &v.rx, &v.ry, &v.dx, &v.dy);
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
    touch_reset(); g_s = {}; g_s.loaded = true; g_s.layout = defaults(0); g_s.saved = g_s.layout;
    profile = 0; weapon_override = -1; in_quest = true; g_s.in_quest = true; g_s.detected_weapon = 0;
    g_s.touched = true; g_s.w = (float)w; g_s.h = (float)h; g_s.sel = -1;
    cfg = {}; cfg.touch_controls = TOUCH_ON; cfg.touch_auto_r = 1;
    const int shown[] = TOUCH_SHOW_DEFAULT; std::copy(shown, shown + TB_COUNT, cfg.touch_show);
    settings_open = false; drawn = 0; ranged = 0; bow_type = 0; aiming = lock_on = persistent_lock_on = false; hunt_camera = true;
    target_cycles = 0; item_steps.clear();
    g_lock_follow = g_lock_shots = false;
    g_visible = false; g_gear_only = false; g_style = g_s.layout.style; g_ranged = 0;
    g_zones = {}; g_build = {}; g_build_menu = false; g_menu_ns = 0; g_press_ns = 0;
    g_play_ns = SDL_GetTicksNS(); g_window_open = g_window_asked = false; g_town_ns = 0;
    update(); assert(!g_s.menu);
}
static void game_menu_state(bool menu) {
    g_menu_ns = menu ? SDL_GetTicksNS() : 0;
    if (menu) g_press_ns = g_menu_ns.load(), g_window_open = false;
    update(); assert(g_s.menu == menu);
}
static ImVec2 hud(int zone, float x, float y) {
    touch_hud_piece(zone, x, y, x, y); touch_hud_frame();
    Rect r = zone_rect(zones(), zone, (float)screen_w, (float)screen_h); assert(r.ok);
    return ImVec2((r.x0 + r.x1) * .5f, (r.y0 + r.y1) * .5f);
}
static int pulses(uint64_t buttons) {
    int n = 0; for (const Pulse &p : g_s.pulse) if (p.until > SDL_GetTicksNS() && p.buttons == buttons) ++n;
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
    event(SDL_EVENT_FINGER_MOTION, 1, p.x + 400, p.y);
    event(SDL_EVENT_FINGER_MOTION, 1, p.x - 300, p.y);
    event(SDL_EVENT_FINGER_UP, 1, p.x - 500, p.y); assert(item_steps == std::vector<int>{1});
    assert(!(update().b & B(BTN_Y)));
    event(SDL_EVENT_FINGER_DOWN, 2, p.x, p.y);
    event(SDL_EVENT_FINGER_UP, 2, p.x - 200, p.y); assert(item_steps == (std::vector<int>{1,-1}));
    event(SDL_EVENT_FINGER_DOWN, 3, p.x, p.y);
    event(SDL_EVENT_FINGER_MOTION, 3, p.x + 25, p.y);
    event(SDL_EVENT_FINGER_UP, 3, p.x + 30, p.y); assert(item_steps.size() == 2); // no lower release threshold
    event(SDL_EVENT_FINGER_DOWN, 4, p.x, p.y); event(SDL_EVENT_FINGER_UP, 4, p.x, p.y);
    assert(update().b & B(BTN_Y));
    fresh(); ranged = WEAPON_BOWGUN; update(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 5, p.x, p.y);
    event(SDL_EVENT_FINGER_MOTION, 5, p.x, p.y - 180); assert(pulses(B(BTN_X)) == 1);
    const uint64_t ready = g_s.ammo_ready;
    event(SDL_EVENT_FINGER_MOTION, 5, p.x, p.y + 200);
    event(SDL_EVENT_FINGER_UP, 5, p.x + 350, p.y - 300);
    assert(pulses(B(BTN_X)) == 1 && pulses(B(BTN_B)) == 0 && item_steps.empty() && g_s.ammo_ready == ready);
    assert(update().b & B(BTN_L));
    fresh(); ranged = WEAPON_BOWGUN; update(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 6, p.x, p.y); event(SDL_EVENT_FINGER_UP, 6, p.x, p.y + 150);
    assert(pulses(B(BTN_B)) == 1 && item_steps.empty());
    puts("PASS: item horizontal/vertical gestures latch one step, reversal and lift, quick down/up flick, tap use");
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
static int drawn_vertices(ImVec2 centre, float r) {
    ImGuiIO &io = ImGui::GetIO(); io.DisplaySize = ImVec2((float)screen_w, (float)screen_h); io.DeltaTime = 1.0f/60;
    ImGui::NewFrame(); touch_draw((float)screen_w, (float)screen_h);
    int n = 0; ImDrawList *list = g_s.edit ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
    for (const ImDrawVert &v : list->VtxBuffer) if (fabsf(v.pos.x-centre.x) < r && fabsf(v.pos.y-centre.y) < r) ++n;
    ImGui::Render(); return n;
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
    cfg.touch_show[TB_DPAD]=0;
    for (int p=1;p<TOUCH_LAYOUT_PROFILES;++p) {
        select_layout(g_s,p);
        assert(profile==p && !cfg.touch_show[TB_DPAD]);
        assert(fabsf(g_s.layout.x[C_DPAD_TOGGLE]-(.2f+p*.015f))<.0001f);
        assert(fabsf(g_s.layout.y[C_DPAD_TOGGLE]-(.76f-p*.008f))<.0001f);
        assert(fabsf(g_s.layout.scale[C_DPAD_TOGGLE]-(.6f+p*.07f))<.001f);
        Geo hunt=place(g_s.layout,C_HDPAD,(float)screen_w,(float)screen_h);
        Geo menu=place(g_s.layout,C_DPAD,(float)screen_w,(float)screen_h);
        assert(hunt.c.x==menu.c.x && hunt.c.y==menu.c.y && hunt.r==menu.r);
    }
    const int legacy=TOUCH_LAYOUT_PROFILES-1; char path[40];layout_path(legacy,path,sizeof path);
    { std::ofstream f(path);f<<"attack 0.3 0.4 1.2\n"; }
    Layout loaded;assert(load_layout(&loaded,legacy));
    assert(fabsf(loaded.x[C_ATTACK]-.3f)<.0001f && fabsf(loaded.scale[C_ATTACK]-1.2f)<.001f);
    assert(loaded.x[C_DPAD_TOGGLE]==DEFS[C_DPAD_TOGGLE].x && loaded.scale[C_DPAD_TOGGLE]==1);
    in_quest=false;update();assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+g_s.legacy_mode);
    in_quest=true;ranged=WEAPON_BOW;update();assert(g_s.layout_profile==profile_for_weapon(10));
    puts("PASS: 15 weapon and 4 legacy profile files, shortcut position/size round trip, shared visibility, old-file defaults and automatic profile switching");
}
static void nonquest_layouts() {
    fresh(); in_quest=false; update();
    for (int menu=0;menu<2;++menu) {
        game_menu_state(menu!=0);
        select_legacy_mode(g_s,0);
        for (int mode=0;mode<TOUCH_LEGACY_LAYOUTS;++mode) {
            assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS+mode);
            assert(g_style==legacy_style(mode));
            assert(in_mode(C_LAYOUT,g_s.menu,false));
            assert(in_mode(C_ATTACK,g_s.menu,false)==(mode==0));
            assert(in_mode(C_HEAVY,g_s.menu,false)==(mode==1));
            assert(in_mode(C_CORNER,g_s.menu,false)==(mode==2));
            assert(in_mode(C_RFIRE,g_s.menu,false)==(mode==3));
            assert(!in_mode(C_MPILL,g_s.menu,false));
            at(SDL_EVENT_FINGER_DOWN,C_LAYOUT); at(SDL_EVENT_FINGER_UP,C_LAYOUT); update();
        }
        assert(g_s.layout_profile==TOUCH_WEAPON_LAYOUTS);
    }
    in_quest=true; update(); game_menu_state(true);
    assert(!in_mode(C_MPILL,true,true) && !in_mode(C_LAYOUT,true,true));
    assert(in_mode(C_HEAVY,true,true) && in_mode(C_DODGE,true,true));
    puts("PASS: all four presets cycle in villages and game menus; quest menus keep the attack buttons");
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
        at(SDL_EVENT_FINGER_DOWN,C_RAIM); assert(update().b==(type?B(BTN_X):B(BTN_R)));
        at(SDL_EVENT_FINGER_MOTION,C_RAIM,1,2,-1); assert(update().dx>0);
        at(SDL_EVENT_FINGER_CANCELED,C_RAIM,1,2,-1); assert(update().b==0);
    }
    puts("PASS: fire stays A without forced scope/seam, native X+A in either order, aim tap/drag/release, scoped menu suppression, draw press cannot reload");
}
static void ammo_repeat() {
    fresh(); ranged=WEAPON_BOWGUN; drawn=1; update();
    for (int control : {C_AMMO_UP,C_AMMO_DOWN}) {
        uint64_t button=control==C_AMMO_UP?B(BTN_X):B(BTN_B);
        at(SDL_EVENT_FINGER_DOWN,control); assert(update().b==B(BTN_L));
        assert(pulses(button)==1);
        for (Pulse &p:g_s.pulse) p={};
        g_s.ammo_ready=SDL_GetTicksNS()-1; g_s.ammo_hold=SDL_GetTicksNS()+300*MS;
        assert(update().b==B(BTN_L)); assert(pulses(button)==1);
        uint64_t ready=g_s.ammo_ready; update(); assert(g_s.ammo_ready==ready && pulses(button)==1);
        at(SDL_EVENT_FINGER_UP,control); g_s.ammo_ready=SDL_GetTicksNS()-1;
        update(); assert(pulses(button)==1);  // No additional steps once the finger lifts.
        touch_reset(); assert(update().b==0 && !g_s.ammo_hold && !g_s.ammo_ready);
    }
    puts("PASS: ammo arrows repeat while held, stop queueing on lift, background reset releases pending L and steps");
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
    const Pair guns[]={{TB_FIRE,C_RFIRE,-1},{TB_SECOND,C_RSECOND,-1},{TB_AIM,C_RAIM,-1},
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
    io.Fonts->AddFontDefaultVector();ImGui::NewFrame();
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
    nonquest_layouts();fixed_quest_menus();ranged_controls();ammo_repeat();target_switch(1280,720);target_switch(2520,1080);target_switch(3840,1080);persistent_target_lock();
    ImGui::DestroyContext();
    fresh();cfg.touch_show[TB_DPAD]=0;preview("touch-host-dpad-hidden-1280x720.png");
    cfg.touch_show[TB_DPAD]=1;update();preview("touch-host-dpad-shown-1280x720.png");
    touch_edit_begin();update();preview("touch-host-editor-1280x720.png");
    fresh(640,360);cfg.touch_show[TB_DPAD]=1;update();preview("touch-host-dpad-shown-640x360.png");
    touch_edit_begin();update();preview("touch-host-editor-640x360.png");
    SDL_DestroyWindow(window);SDL_Quit();
    puts("PASS: all touch control regressions (host stubs, no APK/game execution)");return 0;
}
