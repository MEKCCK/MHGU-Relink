// Host regression: real SDL finger events and ImGui drawing; no game files or APK execution.
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <SDL3/SDL.h>
static bool host_rename_path(const char *oldpath,const char *newpath) {
    const bool ok=SDL_RenamePath(oldpath,newpath);
    if(!ok) fprintf(stderr,"HOST RENAME FAILURE: %s -> %s: errno=%d, SDL=%s\n",oldpath,newpath,errno,SDL_GetError());
    return ok;
}
#define SDL_RenamePath host_rename_path
#include "../runtime/touch.cpp"
#undef SDL_RenamePath

static const uint64_t EXPECTED_ART_INPUTS[3] = {
    B(BTN_UP), B(BTN_RIGHT), B(BTN_DOWN)
};
static uint64_t art_inputs[3];

// Keep the same behavioral harness buildable against the frozen pre-fix runtime.
// Its one shared timestamp must fail per-zone behavior, not fail compilation.
template<typename T> static auto &hud_timestamp(T &stamps,int zone) {
    if constexpr(std::is_array_v<T>) return stamps[zone];
    else return stamps;
}
static void hud_art_slot(float game_y,int slot) {
#ifdef MHGU_TOUCH_BASELINE_NO_ART_SLOT
    // The archived runtime predates the renderer marker API; it observes only the pieces.
    (void)game_y; (void)slot;
#else
    touch_hud_art_slot(game_y,slot);
#endif
}

static SDL_Window *window;
static Config cfg;
static bool settings_open;
static bool config_save_fail, companion_commit_fail;
static int config_saves;
static float touch_opacity = .8f;
static int drawn, ranged, profile, bow_type, weapon_override = -1;
static constexpr int UNKNOWN_WEAPON = -2;
static bool in_quest = true;
static bool aiming, lock_on, persistent_lock_on, hunt_camera = true, hunter_moving = true;
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
static struct {
    bool open=false, slider=false, mix_held=false, pause_fresh=true, map_input=false, mapped=false;
    uint64_t id=123;
    float x=500,y=200,w=90,h=35,dx=120,dy=50,tone=0;
    int cols=2,cells=6,selected=-1,picks=0,selections=0,pages=0,slider_sets=0,confirms=0;
    int touch_selections=0,touch_picks=0,mix_cancels=0,pauses=0,menu_points=0;
    float map[4]={1,0,1,0};
    float pause[4]={738,407,114,36}; // Session's frozen picture-space x/y/width/height.
} test_menu;
extern "C" {
volatile uint64_t g_town_ns;
float g_render_aspect = 16.0f / 9.0f;
float g_ui_anchor_aspect = 16.0f / 9.0f;
uint64_t plat_now_ns(void) { return SDL_GetTicksNS(); }
SDL_Window *ui_window(void) { return window; }
bool host_input_press(const char *button,unsigned frames) {
    assert(!strcmp(button,"A") && frames==12); ++test_menu.confirms; return true;
}
// Routing spy only; the real host_input_menu_point is compiled by run_touch_menu_point.py.
bool host_input_menu_point(float u,float v,float *x,float *y) {
    ++test_menu.menu_points;
    if(!std::isfinite(u) || !std::isfinite(v) || u<0 || u>=1 || v<0 || v>=1) return false;
    if(test_menu.map_input) {
        const double aspect=g_render_aspect>0?g_render_aspect:16.0/9.0;
        if(!std::isfinite(aspect)) return false;
        const double pw=std::min((double)screen_w,screen_h*aspect),ph=pw/aspect;
        const double ox=(screen_w-pw)*.5,oy=(screen_h-ph)*.5,mx=u*screen_w,my=v*screen_h;
        if(mx<ox || mx>=ox+pw || my<oy || my>=oy+ph) return false;
        const double ax=test_menu.mapped?test_menu.map[0]:std::min(1.0,(16.0/9.0)/aspect);
        const double ay=test_menu.mapped?test_menu.map[2]:std::min(1.0,aspect/(16.0/9.0));
        const double bx=test_menu.mapped?test_menu.map[1]:640*(1-ax),by=test_menu.mapped?test_menu.map[3]:360*(1-ay);
        if(!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) || ax<=0 || ay<=0) return false;
        *x=(float)(((mx-ox)*1280/pw-bx)/ax); *y=(float)(((my-oy)*720/ph-by)/ay);
        return std::isfinite(*x) && std::isfinite(*y);
    }
    *x=u*1280; *y=v*720; return true;
}
bool menuptr_open(void) { return test_menu.open; }
bool menuptr_slider_open(void) { return test_menu.open && test_menu.slider; }
uint64_t menuptr_menu_id(void) { return test_menu.slider?0:test_menu.id; }
bool menuptr_slider_hit(float x,float y) { return x>=143 && x<511 && y>=300 && y<340; }
bool menuptr_slider_panel(float x,float y) { return x>=20 && x<666 && y>=214 && y<558; }
void menuptr_slider_set(float x,bool pick) { assert(!pick); test_menu.tone=x; ++test_menu.slider_sets; }
void menuptr_page_turn(int dir) { test_menu.pages+=dir; }
int menuptr_hit(float x,float y) {
    if(!test_menu.open || test_menu.slider) return MENUPTR_NONE;
    if(y>=60 && y<100 && x>=1000 && x<1100) return x<1050?MENUPTR_PAGE_LEFT:MENUPTR_PAGE_RIGHT;
    for(int i=0;i<test_menu.cells;++i) {
        const float x0=test_menu.x+i%test_menu.cols*test_menu.dx,y0=test_menu.y+i/test_menu.cols*test_menu.dy;
        if(x>=x0 && x<x0+test_menu.w && y>=y0 && y<y0+test_menu.h) return i;
    }
    return MENUPTR_NONE;
}
void menuptr_select_at(int cell,bool pick,float x,float y) {
    assert(cell>=0 && cell<test_menu.cells && std::isfinite(x) && std::isfinite(y));
    test_menu.selected=cell; ++test_menu.selections; if(pick) ++test_menu.picks;
}
void menuptr_touch_select_at(int cell,bool pick,float x,float y) {
    ++test_menu.touch_selections; if(pick) ++test_menu.touch_picks;
    menuptr_select_at(cell,pick,x,y);
}
void menuptr_mix_cancel(void) { test_menu.mix_held=false; ++test_menu.mix_cancels; }
uint64_t menuptr_mix_buttons(bool cancel) {
    if(cancel) menuptr_mix_cancel();
    return test_menu.mix_held ? 1 : 0;
}
bool menuptr_mix_pause_region(MenuGeom *out) {
    if(!out || !test_menu.mix_held || !test_menu.pause_fresh) return false;
    *out={}; out->x=test_menu.pause[0]; out->y=test_menu.pause[1]; out->w=test_menu.pause[2]; out->h=test_menu.pause[3]; out->cols=1;
    return true;
}
bool menuptr_mix_pause_at(float x,float y) {
    if(!test_menu.mix_held || !test_menu.pause_fresh || !(x>=test_menu.pause[0] && x<test_menu.pause[0]+test_menu.pause[2] &&
        y>=test_menu.pause[1] && y<test_menu.pause[1]+test_menu.pause[3])) return false;
    test_menu.mix_held=false; ++test_menu.pauses; return true;
}
void menuptr_row_rect(const MenuGeom *g,int cell,int cols,float *x0,float *y0,float *x1,float *y1) {
    assert(g && cell==0 && cols==1);
    *x0=g->x; *x1=g->x+g->w;
    const float extent=std::max(g->h,fabsf(g->dy));
    *y0=g->y-(extent-g->h)*.5f; *y1=*y0+extent;
}
bool menuptr_screen_map(float m[4]) {
    if(!test_menu.mapped) return false;
    std::copy(std::begin(test_menu.map),std::end(test_menu.map),m); return true;
}
uint32_t weapon_hunter(void) { return local_hunter; }
int weapon_drawn(void) { return drawn; }
static bool sheathing;
int weapon_sheathed(void) { return !drawn && !sheathing; }
int weapon_ranged(uint32_t) { return ranged; }
int weapon_type(void) { return weapon_override == UNKNOWN_WEAPON ? -1 : weapon_override >= 0 ? weapon_override : ranged == WEAPON_BOWGUN ? 4 : ranged == WEAPON_BOW ? 10 : 0; }
int presence_in_quest(void) { return in_quest; }
bool weapon_aiming(uint32_t) { return aiming; }
bool weapon_moving(uint32_t) { return hunter_moving; }
unsigned weapon_action(uint32_t) { return 0; }
int weapon_bow_type(void) { return bow_type; }
uint64_t weapon_art_buttons(int slot) { return slot >= 0 && slot < 3 ? art_inputs[slot] : 0; }
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
bool config_save(const Config *value,char *error,size_t size) {
    ++config_saves;
    if(config_save_fail) { if(size) snprintf(error,size,"injected config failure"); return false; }
    cfg=*value; return true;
}
bool config_save_with_companion(const Config *value,const char *staged,const char *path,char *error,size_t size) {
    ++config_saves;
    if(config_save_fail || companion_commit_fail) {
        if(size) snprintf(error,size,"injected companion commit failure");
        return false;
    }
#ifdef _WIN32
    const bool moved=MoveFileExA(staged,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!moved) {
        const unsigned long code=GetLastError();
        fprintf(stderr,"HOST COMPANION FAILURE: %s -> %s: Windows=%lu\n",staged,path,code);
        if(size) snprintf(error,size,"test companion rename: Windows=%lu",code);
        return false;
    }
#else
    if(rename(staged,path)!=0) {
        if(size) snprintf(error,size,"test companion rename: errno=%d",errno);
        return false;
    }
#endif
    cfg=*value; return true;
}
void config_live_touch_combo_name(int index,char *out,size_t size) {
    assert(index>=0 && index<TOUCH_COMBOS);
    if(size) SDL_utf8strlcpy(out,cfg.touch_combo_name[index],size);
}
int config_language(void) { return cfg.language; }
int config_live_touch_controls(void) { return cfg.touch_controls; }
int config_live_touch_hud(void) { return cfg.touch_hud; }
int config_live_touch_auto_r(void) { return cfg.touch_auto_r; }
int config_live_touch_floating(void) { return cfg.touch_floating; }
int config_live_touch_show(int b) { assert(b >= 0 && b < TB_COUNT); return cfg.touch_show[b]; }
void config_toggle_touch_dpad(void) { cfg.touch_show[TB_DPAD] = !cfg.touch_show[TB_DPAD]; }
int config_live_lock_on(void) { return lock_on; }
int config_live_persistent_lock_on(void) { return persistent_lock_on; }
uint32_t config_combo_mask(const char *value) {
    const char *const names[]={"A","B","X","Y","L3","R3","L","R","ZL","ZR","Plus","Minus","Left","Up","Right","Down"};
    const int bits[]={BTN_A,BTN_B,BTN_X,BTN_Y,BTN_LSTICK,BTN_RSTICK,BTN_L,BTN_R,BTN_ZL,BTN_ZR,BTN_PLUS,BTN_MINUS,BTN_LEFT,BTN_UP,BTN_RIGHT,BTN_DOWN};
    uint32_t mask=0; std::string text=value?value:""; size_t start=0;
    while(start<text.size()) {
        const size_t end=text.find('+',start); const std::string token=text.substr(start,end==std::string::npos?end:end-start);
        bool found=false; for(int i=0;i<16;++i) if(token==names[i]) { mask|=(uint32_t)B(bits[i]); found=true; break; }
        if(!found) return 0;
        if(end==std::string::npos) break; start=end+1;
    }
    return mask;
}
bool config_validate(const Config *value,char *error,size_t size) {
    for(int i=0;i<TOUCH_COMBOS;++i) {
        const char *binding=value->touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][i];
        const int pad=value->touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE][i];
        const char *name=value->touch_combo_name[i];
        if((*binding && !config_combo_mask(binding)) || pad<0 || pad>=PADB_COUNT ||
           strlen(name)>=TOUCH_COMBO_NAME_MAX || strpbrk(name,"\r\n\t")) {
            if(size) snprintf(error,size,"invalid test combo configuration"); return false;
        }
    }
    return true;
}
uint32_t config_live_touch_combo(int p, int i) {
    assert(p>=0 && p<TOUCH_LAYOUT_PROFILES && i>=0 && i<TOUCH_COMBOS);
    return config_combo_mask(cfg.touch_weapon_combo[p][i]);
}
int config_live_touch_profile(void) { return profile; }
void config_set_touch_profile(int value) { profile = value; }
float config_live_touch_camera(void) { return 1; }
float config_live_touch_opacity(void) { return touch_opacity; }
void config_combo_text(uint32_t buttons, char *out, size_t n) {
    const char *text = buttons == (B(BTN_X) | B(BTN_A)) ? "X+A" : buttons == B(BTN_X) ? "X" : buttons == B(BTN_A) ? "A" :
                       buttons == B(BTN_Y) ? "Y" : buttons == (B(BTN_L) | B(BTN_R)) ? "L+R" :
                       buttons == (B(BTN_ZL) | B(BTN_ZR)) ? "ZL+ZR" : "";
    if(*text || !buttons) { if(n) snprintf(out,n,"%s",text); return; }
    const char *const names[]={"A","B","X","Y","L3","R3","L","R","ZL","ZR","Plus","Minus","Left","Up","Right","Down"};
    const int bits[]={BTN_A,BTN_B,BTN_X,BTN_Y,BTN_LSTICK,BTN_RSTICK,BTN_L,BTN_R,BTN_ZL,BTN_ZR,BTN_PLUS,BTN_MINUS,BTN_LEFT,BTN_UP,BTN_RIGHT,BTN_DOWN};
    std::string label;
    for(int i=0;i<16;++i) if(buttons&B(bits[i])) { if(!label.empty()) label+='+'; label+=names[i]; }
    if(n) snprintf(out,n,"%s",label.c_str());
}
}
struct Input { uint64_t b{}, touch_b{}; int lx{}, ly{}, rx{}, ry{}; float dx{}, dy{}; bool run{}; };
static Input update(uint64_t controller = 0, bool pad = false, bool typing = false) {
    Input v; v.b = controller;
    v.touch_b = touch_input(pad, typing, &v.b, &v.lx, &v.ly, &v.rx, &v.ry, &v.dx, &v.dy);
    v.run = g_s.run;
    return v;
}
static Input update_axes(uint64_t buttons,int lx,int ly,int rx,int ry) {
    Input v; v.b=buttons; v.lx=lx; v.ly=ly; v.rx=rx; v.ry=ry;
    v.touch_b=touch_input(true,false,&v.b,&v.lx,&v.ly,&v.rx,&v.ry,&v.dx,&v.dy);
    v.run=g_s.run;
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
    test_menu={};
    touch_opacity=.8f;
    screen_w = w; screen_h = h; assert(SDL_SetWindowSize(window, w, h)); SDL_PumpEvents();
    g_render_aspect=(float)w/h;
    g_ui_anchor_aspect=16.0f/9.0f;
    touch_reset(); g_s = {}; g_s.loaded = true; g_s.layout = defaults(COMMON_BASE); g_s.saved = g_s.layout; g_s.layout_profile=UNIVERSAL_LAYOUT;
    profile = TOUCH_UNIVERSAL_PROFILE; weapon_override = -1; in_quest = true; g_s.in_quest = true; g_s.detected_weapon = 0;
    g_s.touched = true; g_s.w = (float)w; g_s.h = (float)h; g_s.sel = -1;
    cfg = {}; cfg.touch_controls = TOUCH_ON; cfg.touch_auto_r = 0;
    const int shown[] = TOUCH_SHOW_DEFAULT; std::copy(shown, shown + TB_COUNT, cfg.touch_show);
    settings_open = false; config_save_fail=companion_commit_fail=false; config_saves=0; drawn = 0; ranged = 0; bow_type = 0; aiming = lock_on = persistent_lock_on = false; hunt_camera = hunter_moving = true; sheathing = false;
    local_hunter=0; bow_drag={};
    std::copy(std::begin(EXPECTED_ART_INPUTS),std::end(EXPECTED_ART_INPUTS),art_inputs);
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
static void editor_click(int button);
static void menu_finger(Uint32 type,int cell=0,SDL_FingerID id=1,float ox=0,float oy=0) {
    const float x=test_menu.x+(cell%test_menu.cols)*test_menu.dx+test_menu.w*.5f+ox;
    const float y=test_menu.y+(cell/test_menu.cols)*test_menu.dy+test_menu.h*.5f+oy;
    event(type,id,x/1280*screen_w,y/720*screen_h);
}
static void game_menu_taps() {
    for(ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) for(bool quest:{false,true}) {
        fresh((int)size.x,(int)size.y); in_quest=quest; update(); game_menu_state(true); test_menu.open=true;
        for(int cols:{1,2}) {
            test_menu.cols=cols;
            for(int cell=0;cell<test_menu.cells;++cell) {
                const int picks=test_menu.picks, touches=test_menu.touch_selections;
                menu_finger(SDL_EVENT_FINGER_DOWN,cell);
                assert(finger(g_s,1)->kind==F_GAME_MENU && test_menu.selected==cell && test_menu.picks==picks);
                assert(test_menu.touch_selections==touches+1 && test_menu.touch_picks==picks);
                Input v=update(); assert(!v.b && !v.lx && !v.ly && !v.dx && !v.dy);
                menu_finger(SDL_EVENT_FINGER_UP,cell); assert(test_menu.picks==picks+1 && !finger(g_s,1));
                assert(test_menu.touch_selections==touches+2 && test_menu.touch_picks==picks+1);
                menu_finger(SDL_EVENT_FINGER_UP,cell); assert(test_menu.picks==picks+1);
            }
        }
        // A visible virtual button keeps its press even when it overlaps an actual menu row.
        const Geo r=place(g_s.layout,C_R,size.x,size.y);
        test_menu.x=r.c.x/size.x*1280-30; test_menu.y=r.c.y/size.y*720-15;
        test_menu.w=60; test_menu.h=30; test_menu.cells=test_menu.cols=1;
        const int picks=test_menu.picks;
        menu_finger(SDL_EVENT_FINGER_DOWN); assert(finger(g_s,1)->kind==F_BUTTON && finger(g_s,1)->ctl==C_R && update().b==B(BTN_R));
        menu_finger(SDL_EVENT_FINGER_CANCELED); assert(test_menu.picks==picks && !update().b);
        // Hiding virtual controls does not disable direct game-menu taps.
        cfg.touch_controls=TOUCH_OFF; update();
        menu_finger(SDL_EVENT_FINGER_DOWN); menu_finger(SDL_EVENT_FINGER_UP); assert(test_menu.picks==picks+1);
        cfg.touch_controls=TOUCH_AUTO; update(0,true);
        menu_finger(SDL_EVENT_FINGER_DOWN); menu_finger(SDL_EVENT_FINGER_UP); assert(test_menu.picks==picks+2);
        const Geo gear=place(g_s.layout,C_SETTINGS,size.x,size.y);
        test_menu.x=gear.c.x/size.x*1280-30; test_menu.y=gear.c.y/size.y*720-15;
        menu_finger(SDL_EVENT_FINGER_DOWN); assert(finger(g_s,1)->kind==F_BUTTON && finger(g_s,1)->ctl==C_SETTINGS);
        menu_finger(SDL_EVENT_FINGER_UP); assert(settings_open && test_menu.picks==picks+2);
    }
    puts("PASS: direct list/grid menu taps select on DOWN and confirm once on UP at three resolutions in town/quest; visible R retains overlaps while Off/Auto-hidden controls allow the menu tap and preserve settings");
}
static void game_menu_button_coexist() {
    struct Button { int ctl; uint64_t mask; float dx=0,dy=0; };
    const Button buttons[]={{C_HEAVY,B(BTN_A)},{C_LIGHT,B(BTN_X)},{C_DODGE,B(BTN_B)},{C_SHEATHE,B(BTN_Y)},
        {C_L,B(BTN_L)},{C_R,B(BTN_R)},{C_ZL,B(BTN_ZL)},{C_ZR,B(BTN_ZR)},
        {C_MENU,B(BTN_PLUS)},{C_HMINUS,B(BTN_MINUS)},{C_MAP,B(BTN_LSTICK)},{C_R3,B(BTN_RSTICK)},
        {C_HDPAD,DPAD_UP,0,-.65f},{C_HDPAD,DPAD_DOWN,0,.65f},
        {C_HDPAD,DPAD_LEFT,-.65f,0},{C_HDPAD,DPAD_RIGHT,.65f,0},
        {C_COMBO,B(BTN_X)|B(BTN_A)},{C_COMBO+1,B(BTN_L)|B(BTN_R)},
        {C_COMBO+2,B(BTN_ZL)|B(BTN_ZR)},{C_COMBO+3,B(BTN_A)},
        {C_COMBO+4,B(BTN_X)},{C_COMBO+5,B(BTN_Y)}};
    const char *const combos[]={"X+A","L+R","ZL+ZR","A","X","Y"};
    for(ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) for(bool quest:{false,true})
        for(const Button &button:buttons) {
            fresh((int)size.x,(int)size.y); in_quest=quest; update(); game_menu_state(true);
            std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
            for(int i=0;i<TOUCH_COMBOS;++i) strcpy(cfg.touch_weapon_combo[profile][i],combos[i]);
            // Isolate a custom-positioned control on top of a menu cell.
            for(int i=0;i<C_COUNT;++i) g_s.layout.x[i]=g_s.layout.y[i]=-20;
            const int c=button.ctl; g_s.layout.x[c]=size.x/(2*size.y); g_s.layout.y[c]=.5f;
            test_menu.open=true; test_menu.x=490; test_menu.y=240; test_menu.w=300; test_menu.h=240;
            test_menu.cols=test_menu.cells=1;
            const Geo g=place(g_s.layout,c,size.x,size.y);
            const ImVec2 p(g.c.x+button.dx*g.r,g.c.y+button.dy*g.r);
            const ImVec2 menu(500/1280.f*size.x,250/720.f*size.y);
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            assert(finger(g_s,1)->kind==F_BUTTON && finger(g_s,1)->ctl==c);
            assert(update().touch_b==button.mask && !test_menu.selections && !test_menu.picks);
            event(SDL_EVENT_FINGER_DOWN,2,menu.x,menu.y);
            assert(finger(g_s,2)->kind==F_GAME_MENU && update().touch_b==button.mask);
            event(SDL_EVENT_FINGER_UP,2,menu.x,menu.y);
            assert(test_menu.picks==1 && update().touch_b==button.mask && finger(g_s,1)->ctl==c);
            finger(g_s,1)->t0=SDL_GetTicksNS()-MIN_PRESS-1;
            event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
            assert(!update().touch_b && !finger(g_s,1) && !finger(g_s,2));
            // The order of the two fingers is independent, including cancellation.
            event(SDL_EVENT_FINGER_DOWN,2,menu.x,menu.y);
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            assert(finger(g_s,2)->kind==F_GAME_MENU && finger(g_s,1)->ctl==c && update().touch_b==button.mask);
            event(SDL_EVENT_FINGER_CANCELED,1,p.x,p.y);
            assert(!update().touch_b && finger(g_s,2)->kind==F_GAME_MENU);
            event(SDL_EVENT_FINGER_UP,2,menu.x,menu.y); assert(test_menu.picks==2 && !update().touch_b);
            // Moving a menu finger over a key neither presses the key nor confirms after the drag.
            event(SDL_EVENT_FINGER_DOWN,2,menu.x,menu.y);
            event(SDL_EVENT_FINGER_MOTION,2,p.x,p.y);
            assert(finger(g_s,2)->kind==F_GAME_MENU && !update().touch_b);
            event(SDL_EVENT_FINGER_UP,2,p.x,p.y); assert(test_menu.picks==2 && !update().touch_b);
            if(is_combo(c)) cfg.touch_weapon_combo[profile][c-C_COMBO][0]=0;
            else cfg.touch_show[show_setting(c)]=0;
            update(); event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            assert(finger(g_s,1)->kind==F_GAME_MENU && !update().touch_b);
            event(SDL_EVENT_FINGER_UP,1,p.x,p.y); assert(test_menu.picks==3 && !update().touch_b);
        }
    puts("PASS: every ordinary button, four D-pad directions and six custom-positioned combos retain menu overlaps; either finger order permits an independent menu tap, release/cancel never sticks, and hidden/empty controls pass through");
}
static void game_menu_stick_coexist() {
    for(bool floating:{false,true}) {
        fresh(); game_menu_state(true); cfg.touch_floating=floating; update();
        for(int i=0;i<C_COUNT;++i) g_s.layout.x[i]=g_s.layout.y[i]=-20;
        g_s.layout.x[C_STICK]=.3f*screen_w/screen_h; g_s.layout.y[C_STICK]=.4f;
        const Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        test_menu.open=true; test_menu.x=g.c.x-40; test_menu.y=g.c.y-30;
        test_menu.w=80; test_menu.h=60; test_menu.cols=test_menu.cells=1;
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        if(floating) {
            assert(finger(g_s,1)->kind==F_GAME_MENU && !g_s.stick_on);
            event(SDL_EVENT_FINGER_UP,1,g.c.x,g.c.y); assert(test_menu.picks==1);
            const ImVec2 base(screen_w*.44f,screen_h*.36f);
            event(SDL_EVENT_FINGER_DOWN,1,base.x,base.y);
            event(SDL_EVENT_FINGER_MOTION,1,base.x+2*g.r,base.y);
            Input v=update(); assert(finger(g_s,1)->kind==F_STICK && g_s.stick_on && v.lx>32000 && !v.ly);
            const ImVec2 knob(g_s.kx+g.r*.4f,g_s.ky);
            assert(hypotf(knob.x-base.x,knob.y-base.y)>g.r*1.22f && knob.x>screen_w*.45f);
            const int selections=test_menu.selections, picks=test_menu.picks;
            for(const ImVec2 second:{base,knob}) for(Uint32 end:{SDL_EVENT_FINGER_UP,SDL_EVENT_FINGER_CANCELED}) {
                test_menu.x=second.x-40; test_menu.y=second.y-30;
                event(SDL_EVENT_FINGER_DOWN,2,second.x,second.y);
                assert(finger(g_s,2)->kind==F_MENU && finger(g_s,1)->kind==F_STICK);
                event(SDL_EVENT_FINGER_MOTION,2,second.x+200,second.y-100);
                v=update();
                assert(g_s.stick_on && v.lx>32000 && !v.ly && !v.b && !v.rx && !v.ry && !v.dx && !v.dy);
                assert(fabsf(g_s.bx-base.x)<.02f && fabsf(g_s.by-base.y)<.02f &&
                       fabsf(g_s.kx-base.x-g.r)<.02f && fabsf(g_s.ky-base.y)<.02f);
                event(end,2,second.x+300,second.y-150);
                v=update();
                assert(!finger(g_s,2) && finger(g_s,1)->kind==F_STICK && g_s.stick_on && v.lx>32000);
                assert(!v.ly && !v.b && !v.rx && !v.ry && !v.dx && !v.dy);
                assert(test_menu.selections==selections && test_menu.picks==picks);
            }
            event(SDL_EVENT_FINGER_UP,1,base.x+2*g.r,base.y);
            v=update();
            assert(!g_s.stick_on && !finger(g_s,1) && !v.b && !v.lx && !v.ly && !v.rx && !v.ry && !v.dx && !v.dy);
        } else {
            assert(finger(g_s,1)->kind==F_STICK && g_s.stick_on && !test_menu.selections);
            event(SDL_EVENT_FINGER_MOTION,1,g.c.x+2*g.r,g.c.y);
            assert(update().lx>30000 && !test_menu.picks);
            event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+2*g.r,g.c.y);
            assert(!g_s.stick_on && !update().lx);
        }
    }
    puts("PASS: the visible fixed stick retains a menu overlap; hidden floating activation permits menu taps, while second touches on its active base/protruding knob never take movement or menu ownership and release/cancel preserves the primary stick");
}
static void game_menu_gesture_lifecycle() {
    for(int mode=0;mode<10;++mode) {
        fresh(); game_menu_state(true); test_menu.open=true;
        menu_finger(SDL_EVENT_FINGER_DOWN); assert(finger(g_s,1)->kind==F_GAME_MENU);
        if(mode==0) menu_finger(SDL_EVENT_FINGER_CANCELED);
        if(mode==1) menu_finger(SDL_EVENT_FINGER_MOTION,2);
        if(mode==2) { ++test_menu.id; update(); }
        if(mode==3) { test_menu.open=false; update(); test_menu.open=true; }
        if(mode==4) { settings_open=true; update(); settings_open=false; }
        if(mode==5) { update(0,false,true); update(); }
        if(mode==6) { touch_edit_begin(); update(); editor_click(E_CANCEL); update(); }
        if(mode==7) { touch_reset(); update(); }
        if(mode==8) { assert(touch_template_save(39,"点击复位")); assert(touch_template_use(39)); update(); }
        if(mode==9) { in_quest=false; update(); }
        menu_finger(SDL_EVENT_FINGER_UP,mode==1?2:0); assert(test_menu.picks==0);
        const Input v=update(); assert(!v.b && !v.lx && !v.ly && !v.dx && !v.dy);
    }
    fresh(); game_menu_state(true); test_menu.open=true;
    menu_finger(SDL_EVENT_FINGER_DOWN); menu_finger(SDL_EVENT_FINGER_UP,2); assert(!test_menu.picks);
    menu_finger(SDL_EVENT_FINGER_DOWN); event(SDL_EVENT_FINGER_UP,1,-10,-10); assert(!test_menu.picks);
    // A second finger on the menu cannot confirm or take over after the first one releases.
    menu_finger(SDL_EVENT_FINGER_DOWN); menu_finger(SDL_EVENT_FINGER_DOWN,1,2);
    assert(finger(g_s,2)->kind==F_GAME_MENU && finger(g_s,2)->done);
    menu_finger(SDL_EVENT_FINGER_UP); assert(test_menu.picks==1);
    menu_finger(SDL_EVENT_FINGER_MOTION,1,2); menu_finger(SDL_EVENT_FINGER_UP,1,2); assert(test_menu.picks==1);
    // Unknown menus and blank areas keep the old directional-swipe behavior, without blindly confirming.
    test_menu.open=false; event(SDL_EVENT_FINGER_DOWN,3,640,100);
    assert(finger(g_s,3)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_MOTION,3,640,190); assert(update().b&B(BTN_DOWN));
    event(SDL_EVENT_FINGER_UP,3,640,190); assert(test_menu.picks==1);
    puts("PASS: menu taps never confirm after drag/flick/outside/cancel, changed/closed menus, overlays, typing, editor, reset, template or quest changes; secondary fingers stay suppressed and unknown menus retain directional swipes");
}
static void game_menu_pages_and_slider() {
    fresh(); game_menu_state(true); test_menu.open=true;
    for(int dir:{-1,1}) {
        const float x=dir<0?1020:1070;
        event(SDL_EVENT_FINGER_DOWN,1,x,80); assert(test_menu.pages==0);
        event(SDL_EVENT_FINGER_UP,1,x,80); assert(test_menu.pages==dir);
        test_menu.pages=0;
    }
    cfg.touch_show[TB_DPAD]=0; update(); // Keep this slider-only fixture clear of the visible virtual D-pad.
    test_menu.slider=true;
    event(SDL_EVENT_FINGER_DOWN,1,300,320);
    assert(finger(g_s,1)->kind==F_GAME_MENU && test_menu.slider_sets==1 && !test_menu.picks);
    event(SDL_EVENT_FINGER_MOTION,1,550,400); assert(test_menu.tone==550 && !update().b);
    event(SDL_EVENT_FINGER_UP,1,570,400); assert(test_menu.tone==570 && !test_menu.picks && !update().b);
    event(SDL_EVENT_FINGER_DOWN,1,100,240); event(SDL_EVENT_FINGER_UP,1,100,240);
    assert(test_menu.confirms==1 && !update().b);
    for(int mode:{TOUCH_OFF,TOUCH_AUTO}) {
        cfg.touch_controls=mode; update(0,true);
        event(SDL_EVENT_FINGER_DOWN,1,100,240); event(SDL_EVENT_FINGER_UP,1,100,240);
        assert(test_menu.confirms==(mode==TOUCH_OFF?2:3) && !update(0,true).b);
    }
    fresh(); game_menu_state(true); cfg.touch_show[TB_DPAD]=0; update(); test_menu.open=true; test_menu.slider=true;
    event(SDL_EVENT_FINGER_DOWN,1,300,320); test_menu.slider=false;
    event(SDL_EVENT_FINGER_UP,1,300,320); assert(!test_menu.picks && !update().b);
    puts("PASS: direct page-arrow taps page once; skin-tone slider follows the finger without camera/A input, panel taps confirm and changed slider screens cancel");
}
static void game_menu_background_resume() {
    fresh(); game_menu_state(true);
    g_play_ns = plat_now_ns();
    g_window_open = g_window_asked = false;
    g_menu_ns = 0;  // Android can suspend longer than the native menu's timestamp window
    touch_background();
    test_menu.open = false;
    update(); assert(!g_s.menu && g_resume_menu.load());  // normal gameplay detection must not consume the pending menu
    event(SDL_EVENT_FINGER_DOWN,1,640,100); assert(finger(g_s,1)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_UP,1,640,100); update();
    touch_hud_menu(); touch_hud_frame(); update();
    assert(g_s.menu);
    event(SDL_EVENT_FINGER_DOWN,1,640,100); assert(finger(g_s,1)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_MOTION,1,640,190);
    assert(update().b & B(BTN_DOWN));
    event(SDL_EVENT_FINGER_UP,1,640,190); update();
    puts("PASS: an already-open native menu remains swipe-navigable after background touch reset and its first resumed HUD frame");
}
static void game_menu_mix_cancel_lifecycle() {
    for(int mode=0;mode<7;++mode) {
        fresh(); game_menu_state(true); test_menu.open=true;
        if(mode==5) assert(touch_template_save(38,"调和取消"));
        test_menu.mix_held=true;
        const int cancels=test_menu.mix_cancels;
        switch(mode) {
            case 0: touch_reset(); break;
            case 1: touch_background(); break;
            case 2: settings_open=true; break;
            case 3: break; // typing is supplied to update below
            case 4: touch_edit_begin(); break;
            case 5: assert(touch_template_use(38)); break;
            case 6: in_quest=false; break;
        }
        const uint64_t physical=B(BTN_X)|B(BTN_R);
        Input v=update(physical,false,mode==3);
        assert(!test_menu.mix_held && test_menu.mix_cancels>cancels);
        assert(v.b==physical && !v.touch_b && !v.lx && !v.ly && !v.dx && !v.dy);
        settings_open=false;
        if(g_s.edit) { editor_click(E_CANCEL); update(); }
        v=update(physical);
        assert(!test_menu.mix_held && v.b==physical && !v.touch_b && !test_menu.picks);
    }
    puts("PASS: continuous Combine ownership cancels on reset, background, settings, typing, editor and template/quest changes without clearing physical input or restarting after resume");
}
static ImVec2 hud(int zone, float x, float y) {
    if(zone==TZ_ARTS) hud_art_slot(y,0);
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
    for(int kind:{WEAPON_BOWGUN,WEAPON_BOW}) {
    fresh(); ranged = kind; local_hunter = 0x1234; update(); p = hud(TZ_ITEM, 640, 620);
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
    fresh(); ranged = kind; local_hunter = 0x1234; update(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 6, p.x, p.y); event(SDL_EVENT_FINGER_UP, 6, p.x, p.y + 150);
    assert(pulses(B(BTN_B)) == 1 && item_steps.empty() && update().b == B(BTN_L));
    fresh(); ranged = kind; local_hunter = 0x1234; in_quest = false; update(); p = hud(TZ_ITEM, 640, 620);
    event(SDL_EVENT_FINGER_DOWN, 7, p.x, p.y);
    event(SDL_EVENT_FINGER_MOTION, 7, p.x, p.y - 180);
    event(SDL_EVENT_FINGER_MOTION, 7, p.x, p.y + 180);
    assert(!pulses(B(BTN_X)) && !pulses(B(BTN_B)) && item_steps.empty());
    event(SDL_EVENT_FINGER_CANCELED, 7, p.x, p.y + 180); assert(!update().b);
    }
    puts("PASS: continuous item, Bowgun ammo and Bow coating slides reverse under the same finger; town does not switch ammo/coatings; long hold opens L and short tap uses Y");
}
static void auto_r() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)})
    for(int floating:{0,1}) for(bool quest:{false,true}) for(int out:{0,1}) for(int flag:{0,1}) {
        fresh((int)size.x,(int)size.y); assert(!cfg.touch_auto_r);
        cfg.touch_auto_r=flag; cfg.touch_floating=floating; in_quest=quest; drawn=out; update();
        const Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        const ImVec2 origin=floating?ImVec2(screen_w*.35f,screen_h*.75f):g.c;
        event(SDL_EVENT_FINGER_DOWN,1,origin.x,origin.y); assert(finger(g_s,1)->kind==F_STICK);
        auto check=[&](Input v,bool expected) {
            const bool run=flag&&!out&&expected;
            assert(v.run==run && g_s.run==run);
            assert(v.b==(run?B(BTN_R):0) && v.touch_b==(run?B(BTN_R):0));
            assert(!v.rx && !v.ry && !v.dx && !v.dy);
        };
        auto move=[&](float radius,bool expected) {
            event(SDL_EVENT_FINGER_MOTION,1,origin.x+radius*g.r,origin.y);
            check(update(),expected);
        };
        check(update(),false);
        move(1.55f,false);
        move(1.6001f,true);
        move(1.5001f,true);
        move(1.4499f,false);
        move(1.6001f,true);
        drawn=1; check(update(),false);          // weapon out: R would be a guard/attack
        drawn=out; move(1.6001f,true);
        // Fully sheathed with Y held (the item key) still runs; only the full sheathe state gates R.
        if (!out && flag) {
            Input y = update(B(BTN_Y));
            assert(y.run && g_s.run && (y.b & B(BTN_R)) && (y.touch_b & B(BTN_R)));
            sheathing = true;                    // a sheathe animation started (or Valor's hold): no R yet
            Input s = update();
            assert(!s.run && !g_s.run && !((s.b | s.touch_b) & B(BTN_R)));
            sheathing = false;                   // fully sheathed: running resumes
            move(1.6001f,true);
        }
        event(SDL_EVENT_FINGER_UP,1,origin.x+1.6001f*g.r,origin.y);
        check(update(),false); assert(!update().lx && !update().ly);
        event(SDL_EVENT_FINGER_DOWN,2,origin.x,origin.y);
        event(SDL_EVENT_FINGER_MOTION,2,origin.x+1.6001f*g.r,origin.y); check(update(),true);
        event(SDL_EVENT_FINGER_CANCELED,2,origin.x+1.6001f*g.r,origin.y); check(update(),false);
    }
    puts("PASS: optional AutoR holds source-marked native R past 1.60 rim radius (1.45 hold) for fixed/floating sticks in town/quests, never with the weapon drawn, and clears on release at four sizes");
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
    event(SDL_EVENT_FINGER_MOTION,2,x+g.r*2,y);assert(update().lx>30000&&!(update().b&B(BTN_R)));
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
    puts("PASS: with AutoR off, common fixed/floating stick origin, neutral landing, broader floating area, item priority and menu movement keep linear movement without synthesized R");
}
static std::vector<ImVec2> touch_geometry() {
    ImGuiIO &io = ImGui::GetIO(); io.DisplaySize = ImVec2((float)screen_w, (float)screen_h); io.DeltaTime = 1.0f/60;
    ImGui::NewFrame(); touch_draw((float)screen_w, (float)screen_h);
    std::vector<ImVec2> vertices; ImDrawList *list = g_s.edit ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
    for (const ImDrawVert &v:list->VtxBuffer) vertices.push_back(v.pos);
    ImGui::Render(); return vertices;
}
static std::vector<ImDrawVert> pause_draw_vertices(float w,float h) {
    ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=ImVec2(w,h); io.DeltaTime=1.0f/60;
    ImGui::NewFrame(); touch_draw(w,h);
    const ImDrawList *list=ImGui::GetBackgroundDrawList();
    std::vector<ImDrawVert> vertices(list->VtxBuffer.begin(),list->VtxBuffer.end());
    ImGui::Render(); return vertices;
}
static ImVec2 pause_screen_point(float x,float y,float w,float h) {
    // Independent expected coordinates: an already-frozen picture-space row inside the fitted game picture.
    const double aspect=g_render_aspect>0?g_render_aspect:16.0/9.0;
    const double pw=std::min((double)w,h*aspect),ph=pw/aspect;
    return ImVec2((float)((w-pw)*.5+x*pw/1280),(float)((h-ph)*.5+y*ph/720));
}
static void freeze_pause_region() {
    // Model only the API contract: snapshot the original row once, before result windows update the global map.
    const double aspect=g_render_aspect>0?g_render_aspect:16.0/9.0;
    const double ax=test_menu.mapped?test_menu.map[0]:std::min(1.0,(16.0/9.0)/aspect);
    const double ay=test_menu.mapped?test_menu.map[2]:std::min(1.0,aspect/(16.0/9.0));
    const double bx=test_menu.mapped?test_menu.map[1]:640*(1-ax),by=test_menu.mapped?test_menu.map[3]:360*(1-ay);
    test_menu.pause[0]=(float)(ax*738+bx); test_menu.pause[1]=(float)(ay*407+by);
    test_menu.pause[2]=(float)(ax*114); test_menu.pause[3]=(float)(ay*36);
}
static void game_menu_mix_pause_finger() {
    for(Uint32 ending:{SDL_EVENT_FINGER_UP,SDL_EVENT_FINGER_CANCELED}) {
        fresh(); assert(!g_s.menu && !menuptr_open());
        test_menu.mix_held=true;
        event(SDL_EVENT_FINGER_DOWN,1,795,425); // The native cursor can be absent throughout its result animation.
        assert(!test_menu.mix_held && test_menu.pauses==1);
        assert(finger(g_s,1)->kind==F_MENU && !finger(g_s,1)->part);
        Input v=update(); assert(!v.b && !v.touch_b && !v.lx && !v.ly && !v.rx && !v.ry && !v.dx && !v.dy);
        test_menu.open=true; ++test_menu.id; test_menu.x=738; test_menu.y=407;
        game_menu_state(true); // A new native menu/cursor identity must not turn this finger into confirmation.
        event(SDL_EVENT_FINGER_MOTION,1,1000,600);
        event(ending,1,795,425);
        v=update(); assert(!finger(g_s,1) && !v.b && !v.touch_b && !v.lx && !v.ly && !v.rx && !v.ry && !v.dx && !v.dy);
        assert(!test_menu.picks && !test_menu.selections && !test_menu.confirms && test_menu.pauses==1 && !test_menu.mix_held);
    }
    fresh();
    g_s.layout.x[C_HEAVY]=(from_right(g_s.layout,C_HEAVY)?screen_w-795.0f:795.0f)/screen_h;
    g_s.layout.y[C_HEAVY]=(screen_h-425.0f)/screen_h;
    const Geo overlap=place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h);
    assert(fabsf(overlap.c.x-795)<.01f && fabsf(overlap.c.y-425)<.01f);
    at(SDL_EVENT_FINGER_DOWN,C_R,2); assert(update().b==B(BTN_R));
    test_menu.mix_held=true; test_menu.open=true; ++test_menu.id;
    event(SDL_EVENT_FINGER_DOWN,1,795,425);
    assert(test_menu.pauses==1 && !test_menu.mix_held && finger(g_s,1)->kind==F_MENU && !finger(g_s,1)->part);
    Input v=update(B(BTN_Y)); assert(v.b==(B(BTN_R)|B(BTN_Y)) && v.touch_b==B(BTN_R));
    event(SDL_EVENT_FINGER_MOTION,1,1000,250); event(SDL_EVENT_FINGER_UP,1,1000,250);
    v=update(); assert(v.b==B(BTN_R) && v.touch_b==B(BTN_R) && !v.dx && !v.dy && !v.rx && !v.ry);
    assert(!test_menu.picks && !test_menu.selections && !test_menu.confirms);
    at(SDL_EVENT_FINGER_UP,C_R,2); v=update();
    assert(!g_s.held && !(v.b&~B(BTN_R)) && !(v.touch_b&~B(BTN_R))); // The independent R tap retains its normal short pulse.
    puts("PASS: original Combine row pauses on DOWN during cursor-free animation, owns drag/release across menu identity changes and beats overlapping controls while preserving other fingers and physical buttons");
}
static void game_menu_mix_pause_mapping() {
    struct Case { int w,h; float aspect,dpi; bool mapped,hidden; };
    const Case cases[]={
        {640,360,16.0f/9,1,false,false}, {1280,720,16.0f/9,1,true,true},
        {2520,1080,21.0f/9,1,false,true}, {720,1280,9.0f/16,1,false,true},
        {1280,720,4.0f/3,1,false,true}, {1280,720,0,1,false,true},
        {720,1280,-1,1,false,true}, {1280,720,NAN,1,false,true},
        {1280,720,16.0f/9,2,true,true}, {2520,1080,21.0f/9,1.5f,true,true}
    };
    for(const Case &c:cases) {
        fresh(c.w,c.h); g_render_aspect=c.aspect; test_menu.map_input=true; test_menu.mapped=c.mapped;
        test_menu.map[0]=.8f; test_menu.map[1]=160; test_menu.map[2]=.9f; test_menu.map[3]=20;
        cfg.touch_controls=c.hidden?TOUCH_OFF:TOUCH_ON; touch_opacity=0; update(); g_gear_only=false;
        freeze_pause_region();
        test_menu.mix_held=true; assert(touch_drawn());
        const float w=c.w*c.dpi,h=c.h*c.dpi;
        const ImVec2 lo=pause_screen_point(test_menu.pause[0],test_menu.pause[1],w,h),
            hi=pause_screen_point(test_menu.pause[0]+test_menu.pause[2],test_menu.pause[1]+test_menu.pause[3],w,h);
        const std::vector<ImDrawVert> vertices=pause_draw_vertices(w,h);
        assert(vertices.size()>20);
        const ImVec2 expected[4]={{lo.x,lo.y},{hi.x,lo.y},{hi.x,hi.y},{lo.x,hi.y}};
        for(int i=0;i<4;++i) {
            assert(fabsf(vertices[i].pos.x-expected[i].x)<.03f && fabsf(vertices[i].pos.y-expected[i].y)<.03f);
            assert(((vertices[i].col>>IM_COL32_A_SHIFT)&255)>=230); // Independent of control opacity and idle fading.
        }
        for(const ImDrawVert &v:vertices) {
            const float edge=std::max(2.0f,(hi.y-lo.y)*.06f);
            assert(std::isfinite(v.pos.x) && std::isfinite(v.pos.y));
            assert(v.pos.x>=lo.x-edge && v.pos.x<=hi.x+edge && v.pos.y>=lo.y-edge && v.pos.y<=hi.y+edge);
        }
        // The result window may replace the menu affine transform, then its 200ms cache may expire.
        // Neither can move the session's original plate or send its touch through the current inverse menu map.
        test_menu.mapped=true;
        test_menu.map[0]=.45f; test_menu.map[1]=320; test_menu.map[2]=.6f; test_menu.map[3]=100;
        for(bool current_map:{true,false}) {
            test_menu.mapped=current_map;
            const std::vector<ImDrawVert> unchanged=pause_draw_vertices(w,h);
            assert(unchanged.size()==vertices.size());
            for(size_t i=0;i<vertices.size();++i)
                assert(unchanged[i].pos.x==vertices[i].pos.x && unchanged[i].pos.y==vertices[i].pos.y && unchanged[i].col==vertices[i].col);
        }
        test_menu.mapped=true;
        const ImVec2 shifted=pause_screen_point(.45f*795+320,.6f*425+100,w,h);
        event(SDL_EVENT_FINGER_DOWN,1,shifted.x/c.dpi,shifted.y/c.dpi);
        assert(test_menu.mix_held && !test_menu.pauses);
        event(SDL_EVENT_FINGER_CANCELED,1,shifted.x/c.dpi,shifted.y/c.dpi);
        // Miss immediately outside each edge, then hit near the same edges, all via actual normalized SDL events.
        const ImVec2 outside[4]={{lo.x-2*c.dpi,(lo.y+hi.y)*.5f},{hi.x+2*c.dpi,(lo.y+hi.y)*.5f},
            {(lo.x+hi.x)*.5f,lo.y-2*c.dpi},{(lo.x+hi.x)*.5f,hi.y+2*c.dpi}};
        for(const ImVec2 &p:outside) {
            event(SDL_EVENT_FINGER_DOWN,1,p.x/c.dpi,p.y/c.dpi);
            assert(test_menu.mix_held && !test_menu.pauses);
            event(SDL_EVENT_FINGER_CANCELED,1,p.x/c.dpi,p.y/c.dpi);
        }
        const ImVec2 inside[5]={{lo.x+2*c.dpi,(lo.y+hi.y)*.5f},{hi.x-2*c.dpi,(lo.y+hi.y)*.5f},
            {(lo.x+hi.x)*.5f,lo.y+2*c.dpi},{(lo.x+hi.x)*.5f,hi.y-2*c.dpi},{(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f}};
        for(int i=0;i<5;++i) {
            test_menu.mix_held=true;
            test_menu.mapped=i%2==0; // Changed and expired map must both retain the exact startup hit region.
            const int conversions=test_menu.menu_points;
            event(SDL_EVENT_FINGER_DOWN,1,inside[i].x/c.dpi,inside[i].y/c.dpi);
            assert(!test_menu.mix_held && test_menu.pauses==i+1 && finger(g_s,1)->kind==F_MENU && !finger(g_s,1)->part);
            assert(test_menu.menu_points==conversions);
            event(SDL_EVENT_FINGER_UP,1,inside[i].x/c.dpi,inside[i].y/c.dpi);
        }
        if(c.hidden) assert(pause_draw_vertices(w,h).empty() && !touch_drawn());
    }
    assert(!strcmp(settings_text(2,"Pause combining"),"暂停调和"));
    fresh(); cfg.touch_controls=TOUCH_OFF; update(); g_gear_only=false; test_menu.mix_held=true; test_menu.pause_fresh=false;
    assert(!touch_drawn() && pause_draw_vertices(screen_w,screen_h).empty());
    event(SDL_EVENT_FINGER_DOWN,1,795,425); assert(test_menu.mix_held && !test_menu.pauses);
    event(SDL_EVENT_FINGER_CANCELED,1,795,425);
    for(float invalid:{INFINITY,-1.0f,0.0f,NAN}) {
        test_menu.pause_fresh=true; test_menu.pause[2]=invalid;
        assert(pause_draw_vertices(screen_w,screen_h).empty());
    }
    test_menu.pause[2]=114; g_render_aspect=INFINITY;
    assert(pause_draw_vertices(screen_w,screen_h).empty());
    for(bool portrait:{false,true}) {
        fresh(portrait?720:1280,portrait?1280:720); g_render_aspect=portrait?16.0f/9:4.0f/3;
        cfg.touch_controls=TOUCH_OFF; update(); g_gear_only=false; test_menu.mix_held=true;
        // Even an overlarge region cannot acquire a finger from the picture's letterbox bars.
        test_menu.pause[0]=test_menu.pause[1]=-1000; test_menu.pause[2]=test_menu.pause[3]=4000;
        event(SDL_EVENT_FINGER_DOWN,1,portrait?360:80,portrait?100:360);
        assert(test_menu.mix_held && !test_menu.pauses);
        event(SDL_EVENT_FINGER_CANCELED,1,portrait?360:80,portrait?100:360);
    }
    puts("PASS: frozen startup pause plate and SDL hit boundaries survive changed/expired global menu transforms at small/normal/ultrawide/portrait sizes, letterbox, 16:9 fallback and 1.5x/2x DPI; hidden/zero-opacity stays visible and stale/invalid geometry stays absent");
}
static void game_menu_mix_pause_ownership() {
    for(int owner=0;owner<3;++owner) {
        fresh();
        if(owner==0) { settings_open=true; update(); }
        if(owner==1) update(0,false,true);
        if(owner==2) { touch_edit_begin(); update(); assert(g_s.edit); }
        // Simulate the render/input boundary before the next lifecycle cancellation update.
        test_menu.mix_held=true;
        assert(pause_draw_vertices(screen_w,screen_h).empty());
        event(SDL_EVENT_FINGER_DOWN,1,795,425);
        assert(test_menu.mix_held && !test_menu.pauses);
        if(owner==0) assert(finger(g_s,1)->kind==F_MENU && finger(g_s,1)->part);
        event(SDL_EVENT_FINGER_CANCELED,1,795,425);
        update(0,false,owner==1); assert(!test_menu.mix_held);
        settings_open=false;
        if(g_s.edit) { editor_click(E_CANCEL); update(); }
    }
    puts("PASS: settings mouse, text input and layout editor keep touch/draw ownership over the original-row pause gesture");
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
    at(SDL_EVENT_FINGER_DOWN, C_HDPAD, 2, 0, -.65f);
    const Input dpad=update(); assert(dpad.b==DPAD_UP && dpad.touch_b==DPAD_UP && !dpad.rx && !dpad.ry);
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
    // Hunter Arts use the same direction bits with ZL, and remain available when the D-pad is hidden.
    ImVec2 art = hud(TZ_ARTS, 640, 280);
    event(SDL_EVENT_FINGER_DOWN, 7, art.x, art.y); assert(finger(g_s,7)->kind == F_ARTS);
    event(SDL_EVENT_FINGER_UP, 7, art.x, art.y); assert(update().b == EXPECTED_ART_INPUTS[0]);
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
static void right_stick_visibility() {
    g_right_stick_visible.store(0); fresh();
    g_s.layout = defaults(UNIVERSAL_LAYOUT); g_s.saved = g_s.layout; g_style = STYLE_CLASSIC;
    const Geo toggle = place(g_s.layout, C_RSTICK_TOGGLE, screen_w, screen_h);
    const Geo right = place(g_s.layout, C_RSTICK, screen_w, screen_h);
    assert(in_mode(C_RSTICK_TOGGLE, false, true) && !in_mode(C_RSTICK, false, true));
    assert(drawn_vertices(toggle.c, toggle.r) > 20 && drawn_vertices(right.c, right.r) == 0);
    at(SDL_EVENT_FINGER_DOWN, C_RSTICK_TOGGLE); assert(in_mode(C_RSTICK, false, true));
    at(SDL_EVENT_FINGER_UP, C_RSTICK_TOGGLE);
    at(SDL_EVENT_FINGER_DOWN, C_RSTICK);
    at(SDL_EVENT_FINGER_MOTION, C_RSTICK, 1, .5f, 0);
    const Input aimed = update(); assert(aimed.rx > 16000 && !aimed.ry && !aimed.b && touch_virtual_right_stick_active());
    at(SDL_EVENT_FINGER_DOWN, C_RSTICK_TOGGLE, 2);
    const Input hidden = update(B(BTN_A), true);
    assert(!in_mode(C_RSTICK, false, true) && !finger(g_s, 1) && !touch_virtual_right_stick_active());
    assert(hidden.rx == 0 && hidden.ry == 0 && hidden.b == B(BTN_A));
    at(SDL_EVENT_FINGER_UP, C_RSTICK_TOGGLE, 2); at(SDL_EVENT_FINGER_UP, C_RSTICK, 1);
    assert(drawn_vertices(toggle.c, toggle.r) > 20 && drawn_vertices(right.c, right.r) == 0);
    g_right_stick_visible.store(0);
    for (int style : {STYLE_PILL, STYLE_SEPARATE, STYLE_CORNER}) {
        fresh(); g_s.layout=defaults(0); g_s.saved=g_s.layout; g_s.layout_profile=0; profile=0;
        g_s.layout.style=style; g_style=style; g_right_stick_visible.store(0);
        assert(in_mode(C_RSTICK_TOGGLE,false,true) && !in_mode(C_RSTICK,false,true));
        at(SDL_EVENT_FINGER_DOWN,C_RSTICK_TOGGLE); at(SDL_EVENT_FINGER_UP,C_RSTICK_TOGGLE);
        assert(in_mode(C_RSTICK,false,true));
        at(SDL_EVENT_FINGER_DOWN,C_RSTICK); assert(finger(g_s,1)->kind==F_RSTICK);
        at(SDL_EVENT_FINGER_MOTION,C_RSTICK,1,.5f,0);
        const Input aimed_layout=update(); assert(aimed_layout.rx>16000 && !aimed_layout.ry && touch_virtual_right_stick_active());
        at(SDL_EVENT_FINGER_DOWN,C_RSTICK_TOGGLE,2);
        const Input hidden_layout=update(); assert(hidden_layout.rx==0 && hidden_layout.ry==0 && !finger(g_s,1));
        at(SDL_EVENT_FINGER_UP,C_RSTICK_TOGGLE,2); at(SDL_EVENT_FINGER_UP,C_RSTICK,1);
    }
    g_right_stick_visible.store(0);
    puts("PASS: right-stick visibility toggle captures axes only while shown and releases an active stick when hidden");
}
static void editor_click(int button) {
    Rect panel, btn[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,btn);
    float x=(btn[button].x0+btn[button].x1)*.5f,y=(btn[button].y0+btn[button].y1)*.5f;
    event(SDL_EVENT_FINGER_DOWN,9,x,y); event(SDL_EVENT_FINGER_UP,9,x,y);
}
static ImVec2 editor_header() {
    Rect panel,btn[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,btn);
    return ImVec2((panel.x0+panel.x1)*.5f,(panel.y0+btn[0].y0)*.5f);
}
static void editor_move_panel(float x,float y) {
    assert(g_s.edit);
    Rect panel,btn[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,btn);
    const ImVec2 p=editor_header(); const int selected=g_s.sel;
    const float dx=(x-g_s.layout.editor_x)*(screen_w-panel.x1+panel.x0);
    const float dy=(y-g_s.layout.editor_y)*(screen_h-panel.y1+panel.y0);
    event(SDL_EVENT_FINGER_DOWN,90,p.x,p.y); assert(finger(g_s,90)->kind==F_EDIT_PANEL);
    event(SDL_EVENT_FINGER_MOTION,90,p.x+dx,p.y+dy);
    event(SDL_EVENT_FINGER_UP,90,p.x+dx,p.y+dy);
    assert(fabsf(g_s.layout.editor_x-std::clamp(x,0.f,1.f))<.0001f);
    assert(fabsf(g_s.layout.editor_y-std::clamp(y,0.f,1.f))<.0001f && g_s.sel==selected && !g_s.pinch);
}
static void editor_panel_away(ImVec2 p) {
    Rect panel,btn[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,btn);
    if(panel.in(p.x,p.y)) editor_move_panel(.5f,p.y<screen_h*.5f?1.f:0.f);
}
static void same_layout(const Layout &a, const Layout &b) {
    assert(a.style==b.style && a.weapon_profile==b.weapon_profile);
    assert(fabsf(a.editor_x-b.editor_x)<.0001f && fabsf(a.editor_y-b.editor_y)<.0001f);
    for (int c=0;c<C_COUNT;++c) {
        assert(fabsf(a.x[c]-b.x[c])<.0001f && fabsf(a.y[c]-b.y[c])<.0001f);
        assert(fabsf(a.scale[c]-b.scale[c])<.001f);
    }
    for (int c : ICON_CONTROLS)
        assert(a.icons[c].choice==b.icons[c].choice && !strcmp(a.icons[c].hash,b.icons[c].hash));
    for (int z=0;z<TZ_COUNT;++z) assert(fabsf(a.zx[z]-b.zx[z])<.11f && fabsf(a.zy[z]-b.zy[z])<.11f);
}
static std::string stored_layout_text(int p) {
    char path[40]; layout_path(p,path,sizeof path); std::ifstream f(path); assert(f);
    return std::string(std::istreambuf_iterator<char>(f),{});
}
static std::string read_text(const char *path) {
    std::ifstream f(path,std::ios::binary); assert(f);
    return std::string(std::istreambuf_iterator<char>(f),{});
}
static void write_text(const char *path,const std::string &value) {
    std::ofstream f(path,std::ios::binary); assert(f); f<<value; assert(f.good());
}
static std::string legacy_template(const std::string &current,int version) {
    assert(version>=1 && version<=4);
    std::istringstream input(current); std::string line,result;
    while(std::getline(input,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(version<4 && (line.rfind("hud_map ",0)==0 || line.rfind("editor_panel ",0)==0)) continue;
        if(line.rfind("right_stick_toggle ",0)==0 || line.rfind("lock ",0)==0) continue;
        if(line.rfind("icon ",0)==0 && (version==1 || line.rfind("icon lock ",0)==0 ||
           (version==2 && (line.rfind("icon ammo_up ",0)==0 || line.rfind("icon ammo_down ",0)==0)))) continue;
        if(line.rfind("layout_version ",0)==0) line="layout_version "+std::to_string(version);
        result+=line+'\n';
    }
    return result;
}
static bool save_legacy_layout(const Layout &l,int slot) {
    char path[40]; layout_path(slot,path,sizeof path); FILE *f=fopen(path,"w"); assert(f);
    for(int i=0;i<C_LAID;++i) fprintf(f,"%s %.4f %.4f %.3f\n",DEFS[i].key,l.x[i],l.y[i],l.scale[i]);
    for(int i=0;i<TZ_MAP;++i) fprintf(f,"%s %.1f %.1f 1\n",ZONE_KEY[i],l.zx[i],l.zy[i]);
    fprintf(f,"attack_style %d 0 1\nweapon_profile %d 0 1\n",l.style,l.weapon_profile);
    return fclose(f)==0;
}
static void same_combos(const Config &a,const Config &b) {
    for(int i=0;i<TOUCH_COMBOS;++i) {
        assert(config_combo_mask(a.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][i])==config_combo_mask(b.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][i]));
        assert(a.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE][i]==b.touch_weapon_combo_pad[TOUCH_UNIVERSAL_PROFILE][i]);
        assert(!strcmp(a.touch_combo_name[i],b.touch_combo_name[i]));
    }
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
    const char *path=COMMON_PATH;
    std::ifstream f(path); const std::string text{std::istreambuf_iterator<char>(f),{}};
    assert(text.find("dpad_toggle ")!=std::string::npos);
    Layout loaded; assert(load_layout_file(&loaded,COMMON_PATH));
    assert(fabsf(loaded.x[C_DPAD_TOGGLE]-before.x[C_DPAD_TOGGLE])<.0001f);
    assert(fabsf(loaded.y[C_DPAD_TOGGLE]-before.y[C_DPAD_TOGGLE])<.0001f);
    assert(fabsf(loaded.scale[C_DPAD_TOGGLE]-before.scale[C_DPAD_TOGGLE])<.001f);
    puts("PASS: shortcut editor drag, +/- size, pinch size and layout file round trip");
}

static const char *const UNIVERSAL_COMBOS[TOUCH_COMBOS]={"X+A","L+R","ZL+ZR","A","X","Y"};
static void enable_universal_combos() {
    for (int i=0;i<TOUCH_COMBOS;++i) strcpy(cfg.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][i],UNIVERSAL_COMBOS[i]);
}

static void neutral_sticks(const Input &v) {
    assert(v.lx==0 && v.ly==0 && v.rx==0 && v.ry==0 && v.dx==0 && v.dy==0);
}
static void dpad_fixture(int context) {
    fresh();
    weapon_override=context==1?6:context==2?10:0;
    ranged=context==1?WEAPON_BOWGUN:context==2?WEAPON_BOW:0;
    drawn=1; cfg.touch_show[TB_DPAD]=1; update();
}
static void camera_only(const Input &v,int rx,int ry) {
    assert(!v.b && !v.touch_b && !v.lx && !v.ly && v.rx==rx && v.ry==ry && !v.dx && !v.dy);
}
static void dpad_only(const Input &v,uint64_t buttons) {
    assert(v.b==buttons && v.touch_b==buttons && !v.lx && !v.ly && !v.rx && !v.ry && !v.dx && !v.dy);
}
static void quest_dpad_camera() {
    const float x[]={.75f,-.75f,0,0},y[]={0,0,-.75f,.75f};
    const uint64_t buttons[]={DPAD_RIGHT,DPAD_LEFT,DPAD_UP,DPAD_DOWN};
    for (int layout=0;layout<3;++layout) {
        dpad_fixture(layout);
        for (int dir=0;dir<4;++dir) {
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,x[dir],y[dir]);
            for (int frame=0;frame<3;++frame) dpad_only(update(),buttons[dir]);
            finger(g_s,1)->t0=SDL_GetTicksNS()-MIN_PRESS-1;
            at(SDL_EVENT_FINGER_UP,C_HDPAD,1,x[dir],y[dir]); camera_only(update(),0,0);
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,x[dir],y[dir]);
            at(SDL_EVENT_FINGER_UP,C_HDPAD,1,x[dir],y[dir]);
            assert(pulses(buttons[dir])==1); dpad_only(update(),buttons[dir]);
            for (Pulse &p:g_s.pulse) if(p.dpad) p.until=SDL_GetTicksNS();
            camera_only(update(),0,0);
        }
        at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f); dpad_only(update(),DPAD_RIGHT);
        at(SDL_EVENT_FINGER_MOTION,C_HDPAD,1,-.75f); dpad_only(update(),DPAD_LEFT);
        at(SDL_EVENT_FINGER_MOTION,C_HDPAD,1); camera_only(update(),0,0);
        at(SDL_EVENT_FINGER_MOTION,C_HDPAD,1,0,-.75f); dpad_only(update(),DPAD_UP);
        at(SDL_EVENT_FINGER_CANCELED,C_HDPAD,1,0,-.75f); camera_only(update(),0,0);
        const Input physical=update_axes(DPAD_UP,1234,-2345,3456,-4567);
        assert(physical.b==DPAD_UP && !physical.touch_b && physical.lx==1234 && physical.ly==-2345 && physical.rx==3456 && physical.ry==-4567);
        // Hunter Arts and custom combinations may share the exact same direction bit.
        const ImVec2 art=hud(TZ_ARTS,640,280);
        event(SDL_EVENT_FINGER_DOWN,2,art.x,art.y); assert(finger(g_s,2)->kind==F_ARTS);
        event(SDL_EVENT_FINGER_UP,2,art.x,art.y);
        Input mixed=update(); assert(mixed.b==EXPECTED_ART_INPUTS[0] && mixed.touch_b==EXPECTED_ART_INPUTS[0] && !mixed.rx && !mixed.ry);
        for(Pulse &p:g_s.pulse) p={};
        strcpy(cfg.touch_weapon_combo[profile][0],"Up+A");
        g_s.layout.x[C_COMBO]=.8f; g_s.layout.y[C_COMBO]=.35f;
        at(SDL_EVENT_FINGER_DOWN,C_COMBO,3); assert(finger(g_s,3)->ctl==C_COMBO);
        mixed=update(); assert(mixed.b==(DPAD_UP|B(BTN_A)) && mixed.touch_b==mixed.b && !mixed.rx && !mixed.ry);
        at(SDL_EVENT_FINGER_CANCELED,C_HDPAD,1,0,-.75f);
        mixed=update(); assert(mixed.b==(DPAD_UP|B(BTN_A)) && !mixed.rx && !mixed.ry);
        at(SDL_EVENT_FINGER_CANCELED,C_COMBO,3); camera_only(update(),0,0);

        for (bool tap:{false,true}) {
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f);
            if(tap) at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f);
            game_menu_state(true); camera_only(update(),0,0);
            at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f); camera_only(update(),0,0);
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f);
            Input menu=update(); assert(menu.b==DPAD_RIGHT && menu.touch_b==DPAD_RIGHT); neutral_sticks(menu);
            if(tap) at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f);
            game_menu_state(false); camera_only(update(),0,0);
            at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f); camera_only(update(),0,0);
        }
        if(layout==1) {
            aiming=true; g_menu_ns=SDL_GetTicksNS(); g_press_ns=g_menu_ns.load(); g_window_open=false;
            update(); assert(!g_s.menu);
            at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f); dpad_only(update(),DPAD_RIGHT);
            aiming=false; update(); assert(g_s.menu); camera_only(update(),0,0);
            at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f); game_menu_state(false);
        }
        for (bool tap:{false,true}) {
            for (int cancel=0;cancel<7;++cancel) {
                dpad_fixture(layout);
                at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f);
                if(tap) at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f);
                dpad_only(update(),DPAD_RIGHT);
                switch(cancel) {
                case 0: cfg.touch_show[TB_DPAD]=0; break;
                case 1: cfg.touch_controls=TOUCH_OFF; break;
                case 2: settings_open=true; break;
                case 3: touch_edit_begin(); break;
                case 4: touch_reset(); break;
                case 5: in_quest=false; break;
                case 6:
                    settings_open=true;
                    at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f); // Overlay gets the release before the next input sample.
                    settings_open=false;
                    break;
                }
                camera_only(update(),0,0);
                cfg.touch_show[TB_DPAD]=1; cfg.touch_controls=TOUCH_ON; settings_open=false;
                if(g_s.edit) { editor_click(E_CANCEL); update(); }
                if(!in_quest) { in_quest=true; update(); }
                camera_only(update(),0,0);
                at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f); camera_only(update(),0,0);
            }
        }
    }
    for(int mode=0;mode<1;++mode) {
        fresh(); in_quest=false; update(); cfg.touch_show[TB_DPAD]=1;
        for(int menu=0;menu<2;++menu) {
            game_menu_state(menu!=0);
            for(int dir=0;dir<4;++dir) {
                at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,x[dir],y[dir]);
                const Input held=update(); assert(held.b==buttons[dir] && held.touch_b==buttons[dir]); neutral_sticks(held);
                at(SDL_EVENT_FINGER_CANCELED,C_HDPAD,1,x[dir],y[dir]); camera_only(update(),0,0);
            }
        }
        game_menu_state(false); at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f);
        at(SDL_EVENT_FINGER_UP,C_HDPAD,1,.75f); assert(pulses(DPAD_RIGHT)==1);
        in_quest=true; camera_only(update(),0,0); // A scene change clears the town's pending D-pad pulse.
    }
    for(Pulse &p:g_s.pulse) p.until=SDL_GetTicksNS();
    camera_only(update(),0,0);
    event(SDL_EVENT_FINGER_DOWN,50,screen_w*.5f,screen_h*.5f);
    assert(finger(g_s,50)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_MOTION,50,screen_w*.56f,screen_h*.47f);
    const Input swipe=update(); assert(swipe.dx>0 && swipe.dy<0 && !swipe.rx && !swipe.ry);
    event(SDL_EVENT_FINGER_CANCELED,50,screen_w*.56f,screen_h*.47f); camera_only(update(),0,0);
    puts("PASS: D-pad holds, taps and drags send native direction buttons without camera axes; physical sticks, Hunter Arts and combos survive; independent screen swipes still turn the camera across hunt/menu/town contexts");
}

static void no_stick_camera(const Input &v) {
    assert(!v.dx && !v.dy && !v.rx && !v.ry);
}
static void hunter_art_first_entry() {
    fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
    hunt_camera=hunter_moving=false; aiming=false; g_town_ns=0; g_play_ns=0;
    in_quest=false; local_hunter=0; update(); assert(g_s.menu);
    in_quest=true; local_hunter=0x08004000; drawn=1; update(); assert(g_s.menu);
    auto frame=[]() {
        // The original HUD's 631 row is moved to a clear canvas area; identity is tested separately.
        touch_hud_piece(TZ_ARTS,726,631,640,280,0,0);
        touch_hud_piece(TZ_ARTS,730,637,644,286,0,0);
        hud_art_slot(631,0);
        touch_hud_piece(TZ_ITEM,1000,640,640,640,0,0);
        touch_hud_frame();
    };
    auto point=[]() {
        const Rect r=zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h); assert(r.ok);
        return ImVec2((r.x0+r.x1)*.5f,(r.y0+r.y1)*.5f);
    };
    frame(); const Input entered=update(B(BTN_B),true);
    if(g_s.menu) fprintf(stderr,"ART FIRST ENTRY FAILURE: fresh quest HUD remained menu with no camera/movement/menu round-trip\n");
    assert(!g_s.menu && entered.b==B(BTN_B) && !entered.touch_b); no_stick_camera(entered);
    ImVec2 p=point(); event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_ARTS);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); const Input first=update(B(BTN_B),true);
    const uint64_t art_mask=B(BTN_ZL)|DPAD_UP|DPAD_RIGHT|DPAD_DOWN;
    assert(first.touch_b==EXPECTED_ART_INPUTS[0] && first.b==(first.touch_b|B(BTN_B)));
    no_stick_camera(first); const uint64_t selected=first.touch_b;
    touch_reset(); game_menu_state(true);
    p=point(); event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
    assert(finger(g_s,1)->kind!=F_ARTS);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); const Input menu=update(); assert(!(menu.touch_b&art_mask)); no_stick_camera(menu);
    touch_reset(); g_menu_ns=0; g_window_open=false; frame(); update(); assert(!g_s.menu);
    p=point(); event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_ARTS);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); const Input returned=update();
    assert(returned.touch_b==selected && returned.b==selected); no_stick_camera(returned);
    for(int missing=0;missing<4;++missing) {
        fresh(); hunt_camera=hunter_moving=false; aiming=false; g_town_ns=0; g_play_ns=0;
        in_quest=missing!=0; local_hunter=missing==1?0:0x08004000;
        if(missing!=2) frame();
        if(missing==3) hud_timestamp(g_zones.ns,TZ_ARTS)=SDL_GetTicksNS()-stale_limit()-1;
        update(); assert(g_s.menu);
    }
    puts("PASS: fresh quest HUD enables the first Hunter Art tap without prior movement, camera callbacks or opening a menu; explicit menus still suppress it, closing preserves its direction, and town/no-local-hunter/no-HUD/stale-Art with fresh items cannot invent gameplay");
}
static void universal_floating_stick() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) {
        for(bool menu:{false,true}) {
            fresh((int)size.x,(int)size.y); cfg.touch_auto_r=0; cfg.touch_floating=1; game_menu_state(menu);
            const Geo saved=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
            const ImVec2 p(screen_w*.35f,screen_h*.72f);
            assert(drawn_vertices(saved.c,saved.r*.7f)==0);
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            assert(finger(g_s,1)->kind==F_STICK && g_s.stick_on);
            assert(fabsf(g_s.bx-p.x)<.02f && fabsf(g_s.by-p.y)<.02f);
            Input v=update(); assert(!v.lx && !v.ly && !v.b && !g_s.run); no_stick_camera(v);
            assert(drawn_vertices(p,saved.r*.7f)>20 && drawn_vertices(saved.c,saved.r*.7f)==0);
            event(SDL_EVENT_FINGER_MOTION,1,p.x+saved.r*.25f,p.y);
            v=update(); assert(abs(v.lx-8191)<250 && !v.ly && !v.b && !g_s.run); no_stick_camera(v);
            const Geo attack=place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h);
            event(SDL_EVENT_FINGER_MOTION,1,attack.c.x,attack.c.y);
            v=update(); assert(v.lx && !v.b); no_stick_camera(v);
            event(SDL_EVENT_FINGER_MOTION,1,screen_w+100.f,-100.f);
            v=update(); assert(v.lx && v.ly && !v.b); no_stick_camera(v);
            event(SDL_EVENT_FINGER_MOTION,1,p.x+2*saved.r,p.y);
            v=update(); assert(v.lx>32000 && !v.ly && !v.b); no_stick_camera(v);
            // A second touch in the owned stick/base or broader movement zone must never become a camera finger.
            for(const ImVec2 second:{ImVec2(p.x+5,p.y+5),ImVec2(screen_w*.42f,screen_h*.55f)}) {
                event(SDL_EVENT_FINGER_DOWN,2,second.x,second.y);
                const Finger *extra=finger(g_s,2);
                if(extra && (extra->kind==F_CAMERA || extra->kind==F_STICK))
                    fprintf(stderr,"Left-stick second touch misrouted: %dx%d menu=%d kind=%d at %.1f,%.1f\n",screen_w,screen_h,(int)menu,extra->kind,second.x,second.y);
                assert(!extra || (extra->kind!=F_CAMERA && extra->kind!=F_STICK));
                event(SDL_EVENT_FINGER_MOTION,2,second.x+200,second.y-100);
                v=update(); assert(v.lx>32000 && !v.b && fabsf(g_s.bx-p.x)<.02f && fabsf(g_s.by-p.y)<.02f); no_stick_camera(v);
                event(SDL_EVENT_FINGER_UP,2,second.x+300,second.y-150); no_stick_camera(update());
            }
            // A real game button keeps its own camera gesture even while a different finger moves the hunter.
            at(SDL_EVENT_FINGER_DOWN,C_HEAVY,3); at(SDL_EVENT_FINGER_MOTION,C_HEAVY,3,1,-.5f);
            v=update(); assert(v.lx>32000 && v.b==B(BTN_A));
            if(menu) no_stick_camera(v); else assert(v.dx>0 && v.dy<0 && !v.rx && !v.ry);
            at(SDL_EVENT_FINGER_CANCELED,C_HEAVY,3,1,-.5f); no_stick_camera(update());
            event(SDL_EVENT_FINGER_UP,1,screen_w+500.f,-500.f);
            v=update(); assert(!g_s.stick_on && !v.lx && !v.ly && !v.b); no_stick_camera(v);
            assert(drawn_vertices(p,saved.r*.7f)==0 && drawn_vertices(saved.c,saved.r*.7f)==0);
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_MOTION,1,p.x-2*saved.r,p.y+saved.r);
            no_stick_camera(update()); event(SDL_EVENT_FINGER_CANCELED,1,screen_w+10000.f,-10000.f);
            v=update(); assert(!g_s.stick_on && !v.lx && !v.ly); no_stick_camera(v);
            cfg.touch_floating=0; update(); assert(drawn_vertices(saved.c,saved.r*.7f)>20);
            event(SDL_EVENT_FINGER_DOWN,1,saved.c.x+saved.r*.25f,saved.c.y);
            v=update(); assert(abs(v.lx-8191)<250 && !v.ly && !v.b); no_stick_camera(v);
            assert(fabsf(g_s.bx-saved.c.x)<.02f && fabsf(g_s.by-saved.c.y)<.02f);
            event(SDL_EVENT_FINGER_DOWN,2,saved.c.x,saved.c.y); assert(finger(g_s,2)->kind==F_MENU);
            event(SDL_EVENT_FINGER_MOTION,2,screen_w+100.f,-100.f); no_stick_camera(update());
            event(SDL_EVENT_FINGER_CANCELED,2,screen_w+10000.f,-10000.f); no_stick_camera(update());
            event(SDL_EVENT_FINGER_MOTION,1,screen_w+100.f,-100.f); no_stick_camera(update());
            event(SDL_EVENT_FINGER_CANCELED,1,screen_w+10000.f,-10000.f);
            v=update(); assert(!v.lx && !v.ly); no_stick_camera(v);
            assert(drawn_vertices(saved.c,saved.r*.7f)>20);
        }
    }
    puts("PASS: universal FloatingStick hides idle graphics, lands neutral at the touched origin and preserves linear movement at four screen sizes in hunts/menus; stick owners and secondary movement-zone touches never emit camera/right-stick input, while independent game buttons keep drag-camera; Off restores the saved fixed origin");
}
static void universal_floating_boundaries() {
    fresh(); cfg.touch_auto_r=0; cfg.touch_floating=1;
    std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); cfg.touch_show[TB_STICK]=1; update();
    const ImVec2 points[]={{screen_w*.45f-1,screen_h*.65f},{screen_w*.45f+1,screen_h*.65f},
                          {screen_w*.3f,screen_h*.35f+1},{screen_w*.3f,screen_h*.35f-1}};
    for(int i=0;i<4;++i) {
        event(SDL_EVENT_FINGER_DOWN,1,points[i].x,points[i].y);
        assert(finger(g_s,1)->kind==(i%2?F_CAMERA:F_STICK));
        event(SDL_EVENT_FINGER_CANCELED,1,points[i].x,points[i].y); neutral_sticks(update());
    }
    const ImVec2 edge(screen_w*.44f,screen_h*.36f);
    const float radius=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h).r;
    event(SDL_EVENT_FINGER_DOWN,1,edge.x,edge.y); assert(g_s.stick_on);
    for(const ImVec2 rim:{ImVec2(edge.x+radius*.8f,edge.y),ImVec2(edge.x,edge.y-radius*.8f)}) {
        assert(rim.x>screen_w*.45f || rim.y<screen_h*.35f);
        event(SDL_EVENT_FINGER_DOWN,2,rim.x,rim.y);
        const Finger *extra=finger(g_s,2); assert(!extra || (extra->kind!=F_CAMERA && extra->kind!=F_STICK));
        event(SDL_EVENT_FINGER_MOTION,2,rim.x+100,rim.y-100); neutral_sticks(update());
        event(SDL_EVENT_FINGER_UP,2,rim.x+200,rim.y-200); neutral_sticks(update());
    }
    event(SDL_EVENT_FINGER_UP,1,edge.x,edge.y); neutral_sticks(update());
    g_s.layout.x[C_STICK]=screen_w*.9f/screen_h; g_s.layout.y[C_STICK]=.3f;
    const Geo outside=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
    assert(drawn_vertices(outside.c,outside.r*.7f)==0);
    event(SDL_EVENT_FINGER_DOWN,1,outside.c.x,outside.c.y); assert(finger(g_s,1)->kind==F_CAMERA);
    event(SDL_EVENT_FINGER_CANCELED,1,outside.c.x,outside.c.y);
    for(int floating=0;floating<2;++floating) {
        fresh(); cfg.touch_auto_r=0; cfg.touch_floating=floating;
        std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); cfg.touch_show[TB_STICK]=1;
        g_s.layout.x[C_STICK]=edge.x/screen_h; g_s.layout.y[C_STICK]=1-edge.y/screen_h; update();
        event(SDL_EVENT_FINGER_DOWN,1,edge.x,edge.y);
        event(SDL_EVENT_FINGER_MOTION,1,edge.x+2*radius,edge.y);
        Input v=update(); assert(v.lx>32000 && !v.ly); no_stick_camera(v);
        const ImVec2 knob(g_s.kx+radius*.4f,g_s.ky);
        assert(hypotf(knob.x-g_s.bx,knob.y-g_s.by)>1.22f*radius);
        assert(hypotf(knob.x-g_s.kx,knob.y-g_s.ky)<KNOB*radius && knob.x>screen_w*.45f);
        event(SDL_EVENT_FINGER_DOWN,2,knob.x,knob.y);
        const Finger *extra=finger(g_s,2); assert(!extra || (extra->kind!=F_CAMERA && extra->kind!=F_STICK));
        event(SDL_EVENT_FINGER_MOTION,2,knob.x+200,knob.y-100); no_stick_camera(update());
        event(SDL_EVENT_FINGER_UP,1,edge.x+2*radius,edge.y); neutral_sticks(update());
        event(SDL_EVENT_FINGER_MOTION,2,knob.x+300,knob.y-200); neutral_sticks(update());
        event(SDL_EVENT_FINGER_UP,2,knob.x+400,knob.y-300); neutral_sticks(update());
    }
    puts("PASS: universal floating acquisition follows the left/bottom area and ignores an out-of-area saved position; secondary touches on a visible rim crossing the area boundary or a fixed/floating knob beyond the base never become camera or take over after owner release");
}
static void universal_floating_priority_and_reset() {
    fresh(); cfg.touch_auto_r=0; cfg.touch_floating=1; cfg.touch_show[TB_DPAD]=1;
    const Layout original=g_s.layout;
    const ImVec2 p(screen_w*.35f,screen_h*.72f);
    for(int c:{C_HEAVY,C_HDPAD,C_SETTINGS,C_DPAD_TOGGLE}) {
        g_s.layout=original; cfg.touch_show[TB_DPAD]=1;
        g_s.layout.x[c]=(from_right(g_s.layout,c)?screen_w-p.x:p.x)/screen_h;
        g_s.layout.y[c]=1-p.y/screen_h;
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
        assert(finger(g_s,1)->kind==F_BUTTON && finger(g_s,1)->ctl==c && !g_s.stick_on);
        event(SDL_EVENT_FINGER_CANCELED,1,p.x,p.y); neutral_sticks(update());
    }
    for(int zone:{TZ_ITEM,TZ_ARTS,TZ_TARGET}) {
        fresh(); cfg.touch_auto_r=0; cfg.touch_floating=1;
        const ImVec2 point=hud(zone,p.x,p.y);
        assert(point.x<screen_w*.45f && point.y>screen_h*.35f);
        event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y);
        assert(finger(g_s,1)->kind==(zone==TZ_ITEM?F_ITEM:zone==TZ_ARTS?F_ARTS:F_TARGET) && !g_s.stick_on);
        event(SDL_EVENT_FINGER_CANCELED,1,point.x,point.y); neutral_sticks(update());
    }
    for(int mode=0;mode<6;++mode) {
        fresh(); cfg.touch_auto_r=0; cfg.touch_floating=1;
        const Geo fixed=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_MOTION,1,p.x+2*fixed.r,p.y);
        Input v=update(); assert(v.lx>32000); no_stick_camera(v);
        if(mode==0) settings_open=true;
        else if(mode==1) cfg.touch_controls=TOUCH_OFF;
        else if(mode==2) cfg.touch_show[TB_STICK]=0;
        else if(mode==4) touch_reset();
        else if(mode==5) touch_edit_begin();
        v=update(0,false,mode==3);
        if(g_s.stick_on) fprintf(stderr,"FLOAT RESET FAILURE: mode=%d edit=%d setting=%d show=%d\n",mode,g_s.edit,cfg.touch_controls,cfg.touch_show[TB_STICK]);
        assert(!g_s.stick_on); neutral_sticks(v);
        event(SDL_EVENT_FINGER_UP,1,screen_w+10000.f,-10000.f); neutral_sticks(update(0,false,mode==3));
        settings_open=false; cfg.touch_controls=TOUCH_ON; cfg.touch_show[TB_STICK]=1;
        if(g_s.edit) { editor_click(E_CANCEL); update(); }
        neutral_sticks(update()); assert(drawn_vertices(fixed.c,fixed.r*.7f)==0);
    }
    puts("PASS: actual buttons and item/art/target HUD retain priority over universal floating movement; overlays, Off, ShowStick, typing, reset and editor entry release the owner without stale movement or camera samples");
}
static void common_floating_editor_roundtrip() {
    fresh(); cfg.touch_floating=1;
    g_s.layout.x[C_RSTICK]=.343f; g_s.layout.y[C_RSTICK]=.341f; g_s.layout.scale[C_RSTICK]=1.25f;
    assert(save_layout_file(g_s.layout,COMMON_PATH)); g_s.saved=g_s.layout;
    touch_edit_begin(); update(); assert(g_s.edit);
    const Geo l=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
    assert(drawn_vertices(l.c,l.r*.7f)>20);
    event(SDL_EVENT_FINGER_DOWN,1,l.c.x,l.c.y); assert(g_s.sel==C_STICK);
    event(SDL_EVENT_FINGER_MOTION,1,l.c.x+70,l.c.y-30); event(SDL_EVENT_FINGER_UP,1,l.c.x+70,l.c.y-30);
    editor_click(E_BIGGER); const Layout edited=g_s.layout;
    assert(edited.scale[C_STICK]>1.09f && fabsf(edited.x[C_RSTICK]-.343f)<.0001f && fabsf(edited.y[C_RSTICK]-.341f)<.0001f && edited.scale[C_RSTICK]==1.25f);
    editor_click(E_SAVE); update(); assert(!g_s.edit && g_s.layout_profile==UNIVERSAL_LAYOUT && profile==TOUCH_UNIVERSAL_PROFILE);
    Layout saved; assert(load_layout_file(&saved,COMMON_PATH)); same_layout(saved,edited);
    const Geo moved=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
    assert(drawn_vertices(moved.c,moved.r*.7f)==0);
    cfg.touch_floating=0; update(); assert(drawn_vertices(moved.c,moved.r*.7f)>20);
    cfg.touch_floating=1; update();
    std::ifstream f(COMMON_PATH);
    const std::string text{std::istreambuf_iterator<char>(f),{}}; assert(text.find("stick ")!=std::string::npos && text.find("right_stick ")!=std::string::npos);
    touch_edit_begin(); update(); editor_click(E_RESET); same_layout(g_s.layout,defaults(COMMON_BASE));
    editor_click(E_CANCEL); update(); same_layout(g_s.layout,edited);
    puts("PASS: common floating stick stays visible/editable in the editor; drag/size save, idle hides again and FloatingStick off shows the saved position; hidden right-stick coordinates and Defaults/Cancel round-trip unchanged");
}
static void editor_control_art_present(int c) {
    ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=ImVec2((float)screen_w,(float)screen_h); io.DeltaTime=1.0f/60;
    ImGui::NewFrame(); ImDrawList *dl=ImGui::GetForegroundDrawList();
    control(dl,ImGui::GetFont(),g_s.layout,c,(float)screen_w,(float)screen_h,g_s,1.0f);
    assert(dl->VtxBuffer.Size>0);
    for (const ImDrawVert &v:dl->VtxBuffer) assert(std::isfinite(v.pos.x) && std::isfinite(v.pos.y));
    ImGui::Render();
}

static void button_drag_fixture(int context,int w=1280,int h=720) {
    fresh(w,h); drawn=1; cfg.touch_auto_r=0;
    weapon_override=context==1||context==4?6:context==2?10:0;
    ranged=context==1||context==4?WEAPON_BOWGUN:context==2?WEAPON_BOW:0;
    in_quest=context<3;
    std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
    update(); enable_universal_combos();
    assert(g_s.layout_profile==UNIVERSAL_LAYOUT && g_s.layout.style==STYLE_SEPARATE && profile==TOUCH_UNIVERSAL_PROFILE);
    assert(!g_s.menu && !settings_open && !local_hunter && !bow_drag.held);
}
static void camera_delta(const Input &v,float x,float y) {
    const float k=CAMERA_COUNTS/(float)screen_h;
    assert(fabsf(v.dx-x*k)<.02f && fabsf(v.dy-y*k)<.02f);
}
static void ordinary_button_drag() {
    struct Button { int ctl; uint64_t mask; };
    const Button buttons[]={{C_HEAVY,B(BTN_A)},{C_LIGHT,B(BTN_X)},{C_DODGE,B(BTN_B)},{C_SHEATHE,B(BTN_Y)},
        {C_L,B(BTN_L)},{C_R,B(BTN_R)},{C_ZL,B(BTN_ZL)},{C_ZR,B(BTN_ZR)},
        {C_MENU,B(BTN_PLUS)},{C_HMINUS,B(BTN_MINUS)},{C_MAP,B(BTN_LSTICK)},{C_R3,B(BTN_RSTICK)},
        {C_RFIRE,B(BTN_A)},{C_RSECOND,B(BTN_X)},{C_RAIM,B(BTN_R)},
        {C_RSPECIAL,B(BTN_X)|B(BTN_A)},{C_RDODGE,B(BTN_B)},{C_RSHEATHE,B(BTN_Y)},
        {C_COMBO,B(BTN_X)|B(BTN_A)},{C_COMBO+1,B(BTN_L)|B(BTN_R)},
        {C_COMBO+2,B(BTN_ZL)|B(BTN_ZR)},{C_COMBO+3,B(BTN_A)},{C_COMBO+4,B(BTN_X)},{C_COMBO+5,B(BTN_Y)}};
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) for(int layout=0;layout<5;++layout) {
        button_drag_fixture(layout,(int)size.x,(int)size.y);
        const Layout original=g_s.layout;
        for(const Button &button:buttons) {
            const int c=button.ctl;
            if(!in_mode(c,false,g_s.in_quest)) continue;
            g_s.layout=original;
            if(is_combo(c)) { // Isolate the selected combo from the ranged preset's nearby ammo arrows.
                g_s.layout.x[c]=(float)screen_w/(2*screen_h); g_s.layout.y[c]=.65f;
            }
            const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
            const Finger *f=finger(g_s,1); assert(f && f->kind==F_BUTTON && f->ctl==c && !f->done);
            const int part=f->part;
            Input v=update(); assert(v.b==button.mask && v.touch_b==button.mask); camera_delta(v,0,0);
            const float threshold=.025f*screen_h;
            event(SDL_EVENT_FINGER_MOTION,1,g.c.x+threshold-.5f,g.c.y);
            v=update(); assert(v.b==button.mask && !finger(g_s,1)->moved); camera_delta(v,0,0);
            event(SDL_EVENT_FINGER_MOTION,1,g.c.x+threshold+4,g.c.y-7);
            v=update(); assert(v.b==button.mask && v.touch_b==button.mask); camera_delta(v,threshold+4,-7);
            camera_delta(update(),0,0); // Samples are consumed exactly once.
            const bool ranged_layout=false;
            const int neighbor=ranged_layout?(c==C_RFIRE?C_RSECOND:C_RFIRE):(c==C_HEAVY?C_LIGHT:C_HEAVY);
            const Geo target=place(g_s.layout,neighbor,(float)screen_w,(float)screen_h);
            event(SDL_EVENT_FINGER_MOTION,1,target.c.x,target.c.y);
            v=update(); assert(v.b==button.mask && v.touch_b==button.mask);
            camera_delta(v,target.c.x-g.c.x-threshold-4,target.c.y-g.c.y+7);
            f=finger(g_s,1); assert(f->kind==F_BUTTON && f->ctl==c && f->part==part && !f->done);
            finger(g_s,1)->t0=SDL_GetTicksNS()-MIN_PRESS-1;
            event(SDL_EVENT_FINGER_UP,1,target.c.x+7,target.c.y-5);
            v=update(); assert(!v.b && !v.touch_b); camera_delta(v,7,-5);
            camera_delta(update(),0,0); assert(!bow_drag.begins);
        }
    }
    puts("PASS: every enabled ordinary game button and six combos in all common-layout weapon/scene contexts keeps its original control and button across neighboring keys; the height-scaled threshold includes DOWN displacement, UP includes its final delta, and each sample is consumed once");
}
static void button_drag_release_and_multitouch() {
    for(int layout=0;layout<5;++layout) {
        button_drag_fixture(layout);
        const bool ranged_layout=false;
        const int c=ranged_layout?C_RDODGE:C_HEAVY;
        const uint64_t b=B(ranged_layout?BTN_B:BTN_A);
        const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_UP,1,g.c.x+50,g.c.y-30);
        Input v=update(); assert(v.b==b && v.touch_b==b); camera_delta(v,50,-30);
        touch_reset(); update();
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+10000,g.c.y-10000);
        v=update(); assert(!v.b && !v.touch_b); camera_delta(v,0,0);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+40,g.c.y-20);
        event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+10000,g.c.y-10000);
        v=update(); assert(!v.b && !v.touch_b); camera_delta(v,40,-20);
        camera_delta(update(),0,0);

        const int other=ranged_layout?C_RSHEATHE:C_SHEATHE;
        const Geo g2=place(g_s.layout,other,(float)screen_w,(float)screen_h);
        at(SDL_EVENT_FINGER_DOWN,C_STICK,3); at(SDL_EVENT_FINGER_MOTION,C_STICK,3,2,0);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y); event(SDL_EVENT_FINGER_DOWN,2,g2.c.x,g2.c.y);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+45,g.c.y-25);
        event(SDL_EVENT_FINGER_MOTION,2,g2.c.x+35,g2.c.y+30);
        v=update(B(BTN_ZR),true);
        assert(v.b==(b|B(BTN_Y)|B(BTN_ZR)) && v.touch_b==(b|B(BTN_Y)) && v.lx>30000 && !v.ly);
        camera_delta(v,80,5);
        assert(finger(g_s,1)->ctl==c && finger(g_s,2)->ctl==other && finger(g_s,3)->kind==F_STICK);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+52,g.c.y-29);
        event(SDL_EVENT_FINGER_MOTION,2,g2.c.x+43,g2.c.y+34);
        event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+10000,g.c.y-10000);
        v=update(); assert(v.b==B(BTN_Y) && v.lx>30000); camera_delta(v,15,0);
        finger(g_s,2)->t0=SDL_GetTicksNS()-MIN_PRESS-1;
        event(SDL_EVENT_FINGER_UP,2,g2.c.x+47,g2.c.y+31);
        v=update(); assert(!v.b && v.lx>30000); camera_delta(v,4,-3);
        at(SDL_EVENT_FINGER_CANCELED,C_STICK,3); v=update(); assert(!v.b && !v.lx); camera_delta(v,0,0);
    }
    puts("PASS: down/up-only button flicks keep the short native press and camera delta; CANCEL adds no coordinate delta but preserves accepted motion once; two game buttons, physical buttons and the movement stick stay independent");
}
static void button_drag_lifecycle() {
    for(int layout=0;layout<5;++layout) for(int mode=0;mode<11;++mode) {
        button_drag_fixture(layout);
        const int c=C_HEAVY;
        const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+50,g.c.y-25); // Leave camera displacement pending before cancellation.
        Input stopped;
        switch(mode) {
        case 0:
            g_menu_ns=SDL_GetTicksNS(); g_press_ns=g_menu_ns.load(); g_window_open=false;
            stopped=update(); assert(g_s.menu); break;
        case 1: settings_open=true; stopped=update(); break;
        case 2: cfg.touch_controls=TOUCH_OFF; stopped=update(); break;
        case 3: cfg.touch_show[show_setting(c)]=0; stopped=update(); break;
        case 4: touch_edit_begin(); stopped=update(); break;
        case 5: in_quest=!in_quest; stopped=update(); break;
        case 6: touch_reset(); stopped=update(); break;
        case 7: stopped=update(0,false,true); break;
        case 8: assert(touch_template_save(19,"Input reset") && touch_template_use(19)); stopped=update(); break;
        case 9:
            settings_open=true;
            event(SDL_EVENT_FINGER_UP,1,g.c.x+180,g.c.y-90);
            settings_open=false; stopped=update(); break;
        case 10:
            settings_open=true;
            event(SDL_EVENT_FINGER_MOTION,1,g.c.x+180,g.c.y-90);
            stopped=update(); break;
        }
        camera_delta(stopped,0,0);
        event(SDL_EVENT_FINGER_UP,1,g.c.x+260,g.c.y-140);
        camera_delta(update(0,false,mode==7),0,0);
        settings_open=false; cfg.touch_controls=TOUCH_ON; cfg.touch_show[show_setting(c)]=1;
        if(g_s.edit) { editor_click(E_CANCEL); update(); }
        if(g_s.menu) game_menu_state(false);
        camera_delta(update(),0,0);
    }
    for(int layout=0;layout<3;++layout) {
        button_drag_fixture(layout);
        g_s.layout.x[C_COMBO]=(float)screen_w/(2*screen_h); g_s.layout.y[C_COMBO]=.65f;
        const Geo g=place(g_s.layout,C_COMBO,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y); event(SDL_EVENT_FINGER_MOTION,1,g.c.x+50,g.c.y-25);
        cfg.touch_weapon_combo[profile][0][0]=0;
        Input v=update(); assert(!v.b && !v.touch_b); camera_delta(v,0,0);
        event(SDL_EVENT_FINGER_UP,1,g.c.x+200,g.c.y-100); camera_delta(update(),0,0);
    }
    puts("PASS: pending button-camera movement cannot leak through game menus, overlays including event-only release, hidden controls or combos, Off, typing, editor, scene/preset change or background reset");
}
static void button_drag_exclusions() {
    for(int layout=0;layout<5;++layout) {
        button_drag_fixture(layout);
        for(int c:{C_SETTINGS,C_LAYOUT,C_DPAD_TOGGLE}) {
            if(!in_mode(c,false,g_s.in_quest)) continue;
            const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
            assert(finger(g_s,1)->kind==F_BUTTON && !finger_buttons(*finger(g_s,1)));
            event(SDL_EVENT_FINGER_MOTION,1,g.c.x+90,g.c.y-45); camera_delta(update(),0,0);
            event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+150,g.c.y-80); camera_delta(update(),0,0);
        }
        cfg.touch_show[TB_DPAD]=1; update();
        at(SDL_EVENT_FINGER_DOWN,C_HDPAD,1,.75f,0);
        at(SDL_EVENT_FINGER_MOTION,C_HDPAD,1,0,-.75f);
        camera_delta(update(),0,0); assert(finger(g_s,1)->ctl==C_HDPAD);
        at(SDL_EVENT_FINGER_CANCELED,C_HDPAD); camera_delta(update(),0,0);
        const int c=C_HEAVY;
        game_menu_state(true);
        const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y); const uint64_t held=update().b;
        assert(held==B(BTN_A));
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+90,g.c.y-45);
        Input v=update(); assert(v.b==held && finger(g_s,1)->ctl==c); camera_delta(v,0,0);
        event(SDL_EVENT_FINGER_CANCELED,1,g.c.x+150,g.c.y-80); camera_delta(update(),0,0);
    }
    for(int layout:{0,1,3,4}) for(int floating=0;floating<2;++floating) {
        button_drag_fixture(layout); cfg.touch_floating=floating; update();
        const Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        assert(g_s.stick_on && finger(g_s,1)->kind==F_STICK);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+2*g.r,g.c.y);
        Input v=update(); assert(v.lx>30000 && !v.ly && !v.b); no_stick_camera(v);
        const ImVec2 second=floating?ImVec2(screen_w*.35f,screen_h*.72f):ImVec2(g.c.x+5,g.c.y+5);
        assert(in_stick_zone(g_s,second.x,second.y,Rect{}));
        event(SDL_EVENT_FINGER_DOWN,2,second.x,second.y);
        const Finger *f=finger(g_s,2); assert(f && f->kind!=F_CAMERA && f->kind!=F_STICK);
        event(SDL_EVENT_FINGER_MOTION,2,screen_w+100.f,-100.f);
        v=update(); assert(v.lx>30000 && !v.ly && !v.b); no_stick_camera(v);
        assert(finger(g_s,1)->kind==F_STICK && fabsf(g_s.bx-g.c.x)<.02f && fabsf(g_s.by-g.c.y)<.02f);
        event(SDL_EVENT_FINGER_MOTION,1,screen_w+100.f,-100.f); no_stick_camera(update());
        event(SDL_EVENT_FINGER_UP,1,screen_w+200.f,-200.f);
        v=update(); assert(!g_s.stick_on && !v.lx && !v.ly && !v.b); no_stick_camera(v);
        event(SDL_EVENT_FINGER_MOTION,2,screen_w+200.f,-200.f);
        v=update(); assert(!v.lx && !v.ly && !v.b); no_stick_camera(v);
        event(SDL_EVENT_FINGER_CANCELED,2,screen_w+10000.f,-10000.f); no_stick_camera(update());
    }
    puts("PASS: fixed/floating movement owners and secondary movement-area touches across common-layout weapon/scene contexts never control the camera, steal ownership or promote after owner release");
    button_drag_fixture(0); const ImVec2 p=hud(TZ_ITEM,640,620);
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_MOTION,1,p.x+100,p.y);
    Input v=update(); assert(v.b==B(BTN_L) && touch_item_pending==1); camera_delta(v,0,0);
    event(SDL_EVENT_FINGER_CANCELED,1,p.x+200,p.y); camera_delta(update(),0,0);
    button_drag_fixture(2); strcpy(cfg.touch_weapon_combo[profile][0],"Up+A");
    at(SDL_EVENT_FINGER_DOWN,C_COMBO); at(SDL_EVENT_FINGER_MOTION,C_COMBO,1,2,-1);
    v=update(); assert(v.b==(DPAD_UP|B(BTN_A)) && v.dx>0 && v.dy<0 && !v.rx && !v.ry);
    at(SDL_EVENT_FINGER_CANCELED,C_COMBO,1,2,-1);
    puts("PASS: menus, D-pad, UI-only settings/layout/toggle controls and item/ammo gestures never enter ordinary button-camera drag; directional combo bits still belong to their ordinary combo control");
}
static int bow_drag_fixture(int type=0, bool unused=false, int w=1280, int h=720) {
    fresh(w,h); (void)unused;
    local_hunter=0x08004000; ranged=WEAPON_BOW; weapon_override=10; drawn=1; bow_type=type;
    update(); bow_drag={};
    return type?C_R:C_LIGHT;
}
static void bow_drag_counts(float x,float y) {
    float k=CAMERA_COUNTS/(float)screen_h;
    assert(fabsf(bow_drag.dx-x*k)<.01f && fabsf(bow_drag.dy-y*k)<.01f);
}
static void bow_charge_drag() {
    for(int type=0;type<2;++type) for(bool classic:{false}) for(int w:{1280,2520,3840}) {
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
    puts("PASS: common Bow Type1 X and Type2 R own a separate charge-drag channel; first threshold keeps DOWN displacement, UP keeps its last delta, cancel clears, and no aim key is synthesized");
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
        for(int mode=0;mode<4;++mode) {
            const int c=bow_drag_fixture(type);
            if(mode==0) local_hunter=0;
            else if(mode==1) drawn=0;
            else if(mode==2) { ranged=WEAPON_BOWGUN; weapon_override=6; }
            else { ranged=0; weapon_override=0; }
            update(); at(SDL_EVENT_FINGER_DOWN,c); at(SDL_EVENT_FINGER_MOTION,c,1,2,-1);
            const Input v=update(); assert(!bow_drag.begins && !bow_drag.adds && v.dx>0 && v.b==B(type?BTN_R:BTN_X));
            at(SDL_EVENT_FINGER_CANCELED,c,1,2,-1);
        }
        bow_drag_fixture(type); const int other=type?C_LIGHT:C_R;
        at(SDL_EVENT_FINGER_DOWN,other); at(SDL_EVENT_FINGER_MOTION,other,1,2,-1);
        Input ordinary=update(); assert(!bow_drag.begins && ordinary.dx>0 && ordinary.b==B(type?BTN_X:BTN_R));
        at(SDL_EVENT_FINGER_CANCELED,other,1,2,-1);
        strcpy(cfg.touch_weapon_combo[profile][0],type?"L+R":"X+A");
        g_s.layout.x[C_COMBO]=.8f; g_s.layout.y[C_COMBO]=.3f;
        at(SDL_EVENT_FINGER_DOWN,C_COMBO); at(SDL_EVENT_FINGER_MOTION,C_COMBO,1,2,-1);
        ordinary=update(); assert(!bow_drag.begins && ordinary.dx>0 && ordinary.b==config_live_touch_combo(profile,0));
        at(SDL_EVENT_FINGER_CANCELED,C_COMBO,1,2,-1);
    }
    puts("PASS: common Bow charge ownership stays isolated from a second charge finger, free camera, the other X/R key and combo buttons; actual Bow/drawn/hunter state controls eligibility");
}
static void bow_drag_lifecycle() {
    for(int mode=0;mode<14;++mode) {
        bow_drag_fixture(); Geo g=place(g_s.layout,C_LIGHT,(float)screen_w,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,g.c.x,g.c.y);
        event(SDL_EVENT_FINGER_MOTION,1,g.c.x+50,g.c.y-30); assert(bow_drag.held && bow_drag.adds==1);
        if(mode==0) { settings_open=true; event(SDL_EVENT_FINGER_MOTION,1,g.c.x+70,g.c.y-30); }
        else if(mode==1) game_menu_state(true);
        else if(mode==2) { cfg.touch_controls=TOUCH_OFF; update(); }
        else if(mode==3) update(0,false,true);
        else if(mode==4) touch_reset();
        else if(mode==5) assert(touch_template_save(19,"Input reset") && touch_template_use(19));
        else if(mode==6) { touch_edit_begin(); update(); }
        else if(mode==7) { in_quest=false; update(); }
        else if(mode==8) { local_hunter+=0x1000; update(); }
        else if(mode==9) { bow_type=1; update(); }
        else if(mode==10) { drawn=0; update(); }
        else if(mode==11) { ranged=WEAPON_BOWGUN; update(); }
        else if(mode==12) { cfg.touch_show[TB_ATTACKS]=0; update(); }
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
        const int control=bow_drag_fixture(type); Geo g=place(g_s.layout,control,(float)screen_w,(float)screen_h);
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
        aiming=true; at(SDL_EVENT_FINGER_DOWN,control); at(SDL_EVENT_FINGER_MOTION,control,1,2,-1);
        assert(update().dx>0 && bow_drag.begins==2);
        at(SDL_EVENT_FINGER_CANCELED,control,1,2,-1);
    }
    puts("PASS: manual scope keeps common X/R camera dragging; entering aim cancels charge pending data and leaving it can re-own the same finger using only new displacement");
}

static void ranged_guard_migration() {
    fresh(); const int hbg=4,lbg=5,bow=9; // Legacy file indices, not the active common profile.
    const int sources[]={hbg,lbg,bow,TOUCH_WEAPON_LAYOUTS+3,RESERVED_LAYOUT_BASE};
    for (int show=0;show<2;++show) {
        cfg.touch_show[TB_RANGED]=show;
        for (int slot:sources) {
            Layout old=defaults(reserved_layout(slot)?lbg:slot);
            old.x[C_R]=DEFS[C_R].x; old.y[C_R]=DEFS[C_R].y; old.scale[C_R]=1;
            old.style=STYLE_CORNER;
            assert(save_legacy_layout(old,slot)); Layout loaded; assert(load_layout(&loaded,slot));
            assert(loaded.x[C_R]==.58f && loaded.y[C_R]==.78f && loaded.scale[C_R]==1);
            const std::string original=stored_layout_text(slot);
            assert(load_layout(&loaded,slot) && stored_layout_text(slot)==original);
            for (int custom=0;custom<3;++custom) {
                Layout placed=old;
                if (custom==0) placed.x[C_R]=.317f;
                if (custom==1) placed.y[C_R]=.271f;
                if (custom==2) placed.scale[C_R]=1.17f;
                assert(save_legacy_layout(placed,slot) && load_layout(&loaded,slot));
                assert(loaded.x[C_R]==placed.x[C_R] && loaded.y[C_R]==placed.y[C_R] && loaded.scale[C_R]==placed.scale[C_R]);
            }
            Layout classic=old; classic.style=STYLE_CLASSIC;
            assert(save_legacy_layout(classic,slot) && load_layout(&loaded,slot));
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

static void near_rect(const Rect &a,const Rect &b,float dx=0,float dy=0) {
    assert(a.ok && b.ok);
    assert(fabsf(b.x0-a.x0-dx)<.02f && fabsf(b.x1-a.x1-dx)<.02f);
    assert(fabsf(b.y0-a.y0-dy)<.02f && fabsf(b.y1-a.y1-dy)<.02f);
}
static Rect editor_hud_rect(int zone) {
    return editor_zone_rect(g_s.layout,zone,(float)screen_w,(float)screen_h,g_s.in_quest);
}
static ImVec2 editor_hud_pick(int zone) {
    const Rect r=editor_hud_rect(zone); assert(r.ok);
    editor_panel_away(ImVec2((r.x0+r.x1)*.5f,(r.y0+r.y1)*.5f));
    Rect panel,buttons[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,buttons);
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
    const ImVec2 point=editor_hud_pick(zone); const float x=point.x,y=point.y;
    const Rect before=editor_hud_rect(zone); const Layout layout=g_s.layout;
    event(SDL_EVENT_FINGER_DOWN,1,x,y);
    assert(g_s.sel==ZONE_SEL+zone && finger(g_s,1)->kind==F_EDIT && finger(g_s,1)->ctl==ZONE_SEL+zone);
    event(SDL_EVENT_FINGER_MOTION,1,x+dx,y+dy);
    near_rect(before,editor_hud_rect(zone),dx,dy);  // The handle follows before any renderer feedback arrives.
    for (int k=0;k<TZ_COUNT;++k) {
        assert(g_touch_zone_dx[k]==g_s.layout.zx[k] && g_touch_zone_dy[k]==g_s.layout.zy[k]);
        if (k!=zone) assert(g_s.layout.zx[k]==layout.zx[k] && g_s.layout.zy[k]==layout.zy[k]);
    }
    for (int c=0;c<C_COUNT;++c) assert(g_s.layout.x[c]==layout.x[c] && g_s.layout.y[c]==layout.y[c] && g_s.layout.scale[c]==layout.scale[c]);
    assert(g_s.layout.editor_x==layout.editor_x && g_s.layout.editor_y==layout.editor_y);
    event(SDL_EVENT_FINGER_UP,1,x+dx,y+dy); const Input input=update();
    assert(!input.b && !input.touch_b && !input.lx && !input.ly && !input.dx && !input.dy);
}
static void editor_hud_scenes() {
    for(const ImVec2 size:{ImVec2(1280,720),ImVec2(640,360)}) for(int moved:{0,1}) for(bool quest:{false,true}) {
        fresh((int)size.x,(int)size.y); cfg.touch_hud=moved; in_quest=quest; update();
        for(int k=0;k<TZ_COUNT;++k) { g_s.layout.zx[k]=11.f*(k+1); g_s.layout.zy[k]=-7.f*(k+1); }
        assert(save_layout_file(g_s.layout,COMMON_PATH)); touch_edit_begin(); update();
        const Layout original=g_s.layout;
        for(int cache=0;cache<3;++cache) {
            if(cache==1) {
                for(int k=0;k<TZ_COUNT;++k) {
                    const float x=640,y=260.f+k*90,ox=g_s.layout.zx[k]*squeeze(),oy=g_s.layout.zy[k]*squeeze_y();
                    touch_hud_piece(k,x,y,x+ox,y+oy,ox,oy);
                    if(k==TZ_ARTS) hud_art_slot(y,0);
                }
                touch_hud_frame();
            } else if(cache==2) for(int k=0;k<TZ_COUNT;++k) hud_timestamp(g_zones.ns,k)=SDL_GetTicksNS()-stale_limit()-1;
            for(int k=0;k<TZ_COUNT;++k) {
                assert(editor_hud_rect(k).ok); drag_editor_hud(k,6,4);
            }
        }
        for(int k=0;k<TZ_COUNT;++k)
            assert(g_s.layout.zx[k]!=original.zx[k] && g_s.layout.zy[k]!=original.zy[k]);
        const Layout edited=g_s.layout; editor_click(E_SAVE); update();
        Layout stored; assert(load_layout_file(&stored,COMMON_PATH)); same_layout(stored,edited);
        touch_edit_begin(); update();
        const ImVec2 point=editor_hud_pick(TZ_ITEM);
        event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y); event(SDL_EVENT_FINGER_MOTION,1,point.x+12,point.y+8);
        const Layout draft=g_s.layout,saved=g_s.saved;
        in_quest=!quest; update();
        assert(g_s.edit && g_s.in_quest==!quest && !finger(g_s,1) && g_s.sel==-1 && !g_s.pinch);
        same_layout(g_s.layout,draft); same_layout(g_s.saved,saved);
        assert(load_layout_file(&stored,COMMON_PATH)); same_layout(stored,saved);
        for(int k=0;k<TZ_COUNT;++k) assert(editor_hud_rect(k).ok);
        event(SDL_EVENT_FINGER_MOTION,1,point.x+24,point.y+16); event(SDL_EVENT_FINGER_UP,1,point.x+24,point.y+16);
        same_layout(g_s.layout,draft); editor_click(E_CANCEL); update(); same_layout(g_s.layout,saved);
    }
    puts("PASS: items, Arts, target and the minimap remain editable in town and quests, with absent/live/stale caches, MoveHUD on/off, Save/Cancel and scene transitions");
}
static void editor_hud_dragging() {
    for (const auto size:{ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080),ImVec2(1280,800)}) {
        for (int moved=0;moved<2;++moved) {
            fresh((int)size.x,(int)size.y); cfg.touch_hud=moved;
            assert(save_layout_file(g_s.layout,COMMON_PATH)); update(); touch_edit_begin(); update();
            assert(g_s.edit);
            for (int k=0;k<TZ_COUNT;++k) {
                assert(!g_editor_zones[k].bounds.ok && !zone_rect(zones(),k,(float)screen_w,(float)screen_h).ok);
                drag_editor_hud(k,-24,32); drag_editor_hud(k,13,-11);
            }
            const Layout saved=g_s.layout; editor_click(E_SAVE); update(); assert(!g_s.edit);
            Layout stored; assert(load_layout_file(&stored,COMMON_PATH)); same_layout(stored,saved);
            touch_edit_begin(); update(); drag_editor_hud(TZ_TARGET,-20,18);
            editor_click(E_CANCEL); update(); assert(!g_s.edit); same_layout(g_s.layout,saved);
            touch_reset(); g_s={};
            std::fill(std::begin(g_editor_zones),std::end(g_editor_zones),EditorZone{});
            update(); same_layout(g_s.layout,saved);
            for (int k=0;k<TZ_COUNT;++k) assert(g_touch_zone_dx[k]==g_s.layout.zx[k] && g_touch_zone_dy[k]==g_s.layout.zy[k]);
        }
    }
    puts("PASS: all four HUD handles drag immediately without render feedback, including repeated drags at 16:9/21:9/32:9/16:10 with MoveHUD on/off; buttons stay fixed and offsets survive Save/Cancel/restart");
}
static void editor_hud_feedback() {
    fresh(); cfg.touch_hud=1; update();
    g_s.layout.zx[TZ_ITEM]=60; g_s.layout.zy[TZ_ITEM]=30;
    const ImVec2 base[TZ_COUNT]={{600,500},{830,210},{1080,130},{1170,115}};
    for (int k=0;k<TZ_COUNT;++k) {
        const float ox=g_s.layout.zx[k]*squeeze(),oy=g_s.layout.zy[k]*squeeze_y();
        touch_hud_piece(k,base[k].x,base[k].y,base[k].x+ox,base[k].y+oy,ox,oy);
        if(k==TZ_ARTS) hud_art_slot(base[k].y,0);
    }
    touch_hud_frame(); Rect remembered[TZ_COUNT];
    for (int k=0;k<TZ_COUNT;++k) { remembered[k]=editor_hud_rect(k); assert(g_editor_zones[k].bounds.ok); }
    g_s.layout.zx[TZ_ITEM]+=20; g_s.layout.zy[TZ_ITEM]-=15; publish_zones(g_s.layout);
    near_rect(remembered[TZ_ITEM],editor_hud_rect(TZ_ITEM),20,-15); remembered[TZ_ITEM]=editor_hud_rect(TZ_ITEM);
    touch_hud_piece(TZ_ITEM,base[TZ_ITEM].x,base[TZ_ITEM].y,base[TZ_ITEM].x+80,base[TZ_ITEM].y+15,80,15);
    touch_hud_frame();
    for (int k=0;k<TZ_COUNT;++k) near_rect(remembered[k],editor_hud_rect(k));
    assert(zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h).ok);
    assert(zone_rect(zones(),TZ_TARGET,(float)screen_w,(float)screen_h).ok);
    for(int k=0;k<TZ_COUNT;++k) hud_timestamp(g_zones.ns,k)=SDL_GetTicksNS()-stale_limit()-1;
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
    Layout other=defaults(COMMON_BASE); other.zx[TZ_ITEM]=-35; other.zy[TZ_ITEM]=42;
    const Layout previous=g_s.layout; const Rect old=editor_hud_rect(TZ_ITEM);
    g_s.layout=other; assert(touch_template_save(18,"HUD offset")); g_s.layout=previous;
    assert(touch_template_use(18));
    assert(g_s.layout_profile==UNIVERSAL_LAYOUT && profile==TOUCH_UNIVERSAL_PROFILE);
    const Rect changed=editor_hud_rect(TZ_ITEM);
    near_rect(old,changed,other.zx[TZ_ITEM]-previous.zx[TZ_ITEM],other.zy[TZ_ITEM]-previous.zy[TZ_ITEM]);
    // Incompatible render modes/anchors/aspects use a fresh fallback rather than old-scene coordinates.
    for (int mode=0;mode<3;++mode) {
        if (mode==0) cfg.touch_hud=0;
        if (mode==1) { cfg.touch_hud=1; g_ui_anchor_aspect=21.0f/9.0f; }
        if (mode==2) { g_ui_anchor_aspect=16.0f/9.0f; g_render_aspect=32.0f/9.0f; }
        for (int k=0;k<TZ_COUNT;++k) {
            const Rect actual=editor_hud_rect(k); const EditorZone cache=g_editor_zones[k]; g_editor_zones[k]={};
            const Rect fallback=editor_hud_rect(k); g_editor_zones[k]=cache;
            near_rect(actual,fallback);
        }
    }
    puts("PASS: per-HUD editor caches subtract renderer-applied offsets, survive missing/empty/expired live frames, reapply another preset correctly and reject incompatible mode/anchor/aspect; map hit regions are editor-only");
}
static void map_feedback() {
    const float ox=g_s.layout.zx[TZ_MAP]*squeeze(),oy=g_s.layout.zy[TZ_MAP]*squeeze_y();
    for (const ImVec2 p:{ImVec2(740,0),ImVec2(1100,0),ImVec2(740,360),ImVec2(1100,360)})
        touch_hud_piece(TZ_MAP,p.x,p.y,p.x+ox,p.y+oy,ox,oy);
    touch_hud_frame();
}
static void editor_isolate_hud() {
    for(int c=0;c<C_LAID;++c) g_s.layout.x[c]=3.9f;
}
static void map_editable_scenes() {
    assert(TZ_MAP==3 && TZ_COUNT==4);
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(720,1280)})
    for(int moved:{0,1}) for(bool quest:{false,true}) {
        fresh((int)size.x,(int)size.y); cfg.touch_hud=moved; in_quest=quest; update(); editor_isolate_hud();
        g_s.layout.zx[TZ_MAP]=37; g_s.layout.zy[TZ_MAP]=-19; publish_zones(g_s.layout);
        assert(save_layout_file(g_s.layout,COMMON_PATH));

        // The map handle is available in town and in quests, and stays compact over the whole touch zone.
        Rect fallback=editor_hud_rect(TZ_MAP); assert(fallback.ok);
        assert(fallback.x1-fallback.x0<=size.x*.35f && fallback.y1-fallback.y0<=size.y*.55f);
        map_feedback();
        assert(g_zones.n[TZ_MAP] && g_editor_zones[TZ_MAP].bounds.ok);
        Rect map=editor_hud_rect(TZ_MAP); assert(map.ok);
        assert(map.x1-map.x0<=size.x*.35f && map.y1-map.y0<=size.y*.55f);
        assert(map.x1-map.x0>=size.x*.10f && map.y1-map.y0>=size.y*.10f);

        touch_edit_begin(); update();
        const float zx=g_s.layout.zx[TZ_MAP],zy=g_s.layout.zy[TZ_MAP];
        drag_editor_hud(TZ_MAP,24,16);
        assert(g_s.layout.zx[TZ_MAP]!=zx && g_s.layout.zy[TZ_MAP]!=zy);
        const Layout saved=g_s.layout;
        editor_click(E_SAVE); update(); Layout disk; assert(load_layout_file(&disk,COMMON_PATH)); same_layout(disk,saved);
        assert(touch_template_save(38,"Movable map"));
        g_s.layout.zx[TZ_MAP]=-137; g_s.layout.zy[TZ_MAP]=91; const Layout alternate=g_s.layout;
        assert(touch_template_save(39,"Independent map"));
        assert(touch_template_use(38)); same_layout(g_s.layout,saved);
        assert(touch_template_use(39)); same_layout(g_s.layout,alternate);

        touch_edit_begin(); update(); assert(editor_hud_rect(TZ_MAP).ok);
        editor_click(E_CANCEL); update();
        if (quest) {
            cfg.touch_show[TB_L3]=1; update();
            at(SDL_EVENT_FINGER_DOWN,C_MAP); at(SDL_EVENT_FINGER_UP,C_MAP);
            assert(update().touch_b==B(BTN_LSTICK));
        }
        in_quest=!quest; update();
        touch_edit_begin(); update(); assert(editor_hud_rect(TZ_MAP).ok);
        const Layout before_scene=g_s.layout;
        event(SDL_EVENT_FINGER_DOWN,1,map.x0+(map.x1-map.x0)*.5f,map.y0+(map.y1-map.y0)*.5f);
        event(SDL_EVENT_FINGER_UP,1,map.x0+(map.x1-map.x0)*.5f,map.y0+(map.y1-map.y0)*.5f);
        same_layout(g_s.layout,before_scene);
        editor_click(E_CANCEL); update();
    }
    puts("PASS: map-editable-scenes: a compact minimap handle is editable in town and quests, moves/saves independently per template, preserves L3 map toggle, and survives scene changes at four sizes with MoveHUD on/off");
}

static void hunter_art_zone_lifetime() {
    fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); update();
    touch_hud_piece(TZ_ARTS,726,631,640,280,0,0);
    hud_art_slot(631,0);
    touch_hud_piece(TZ_TARGET,1200,55,1000,120,0,0);
    touch_hud_piece(TZ_ITEM,1000,640,640,620,0,0); touch_hud_frame();
    const Rect arts=zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h);
    const Rect target=zone_rect(zones(),TZ_TARGET,(float)screen_w,(float)screen_h);
    assert(arts.ok && target.ok); const Zones first=zones();
    const ImVec2 p((arts.x0+arts.x1)*.5f,(arts.y0+arts.y1)*.5f);
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_ARTS);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); const uint64_t selected=update().touch_b; assert(selected);
    touch_reset();
    touch_hud_piece(TZ_ITEM,1000,640,660,620,20,0); touch_hud_frame();
    const Rect kept_arts=zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h);
    const Rect kept_target=zone_rect(zones(),TZ_TARGET,(float)screen_w,(float)screen_h);
    if(!kept_arts.ok || !kept_target.ok) fprintf(stderr,"ART ZONE LIFETIME FAILURE: item-only frame erased another live HUD zone\n");
    near_rect(arts,kept_arts); near_rect(target,kept_target);
    assert(hud_timestamp(g_zones.ns,TZ_ARTS)==hud_timestamp(first.ns,TZ_ARTS) &&
           hud_timestamp(g_zones.ns,TZ_TARGET)==hud_timestamp(first.ns,TZ_TARGET));
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_ARTS);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); const Input after_item=update();
    assert(after_item.touch_b==selected && after_item.b==selected); no_stick_camera(after_item); touch_reset();
    hud_timestamp(g_zones.ns,TZ_ARTS)=SDL_GetTicksNS()-stale_limit()-1;
    const uint64_t old_art=hud_timestamp(g_zones.ns,TZ_ARTS);
    touch_hud_piece(TZ_ITEM,1000,640,660,620,20,0); touch_hud_frame();
    assert(hud_timestamp(g_zones.ns,TZ_ARTS)==old_art && !zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h).ok);
    assert(zone_rect(zones(),TZ_TARGET,(float)screen_w,(float)screen_h).ok && zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h).ok);
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind!=F_ARTS);
    event(SDL_EVENT_FINGER_CANCELED,1,p.x,p.y); assert(!update().touch_b);
    touch_hud_piece(TZ_TARGET,1200,55,1010,120,10,0); touch_hud_frame();
    assert(hud_timestamp(g_zones.ns,TZ_ARTS)==old_art && !zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h).ok);
    touch_hud_piece(TZ_ARTS,726,631,660,310,20,30); hud_art_slot(631,0); touch_hud_frame();
    near_rect(arts,zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h),20,30);
    hud_timestamp(g_zones.ns,TZ_TARGET)=SDL_GetTicksNS()-stale_limit()-1;
    const uint64_t old_target=hud_timestamp(g_zones.ns,TZ_TARGET),new_art=hud_timestamp(g_zones.ns,TZ_ARTS);
    touch_hud_piece(TZ_ITEM,1000,640,660,620,20,0); touch_hud_frame();
    assert(hud_timestamp(g_zones.ns,TZ_TARGET)==old_target && !zone_rect(zones(),TZ_TARGET,(float)screen_w,(float)screen_h).ok);
    assert(hud_timestamp(g_zones.ns,TZ_ARTS)==new_art && zone_rect(zones(),TZ_ARTS,(float)screen_w,(float)screen_h).ok);
    const Zones before_empty=zones(); touch_hud_frame();
    for(int k=0;k<TZ_COUNT;++k) assert(hud_timestamp(g_zones.ns,k)==hud_timestamp(before_empty.ns,k));
    puts("PASS: item-only frames preserve fresh Hunter Arts/target positions and Art input; each zone expires by its own timestamp, other-zone/empty frames cannot refresh an expired hit region, and the next real Art piece restores only its new location");
}
static std::vector<ImVec2> hunter_art_frame(const std::vector<int> &slots,bool markers_first) {
    const float sx=squeeze(),sy=squeeze_y();
    const float ox=g_s.layout.zx[TZ_ARTS]*sx,oy=g_s.layout.zy[TZ_ARTS]*sy;
    const float base_y=cfg.touch_hud?180.f:490.f;
    const Pic pic=picture((float)screen_w,(float)screen_h);
    std::vector<ImVec2> points;
    // Compact visible rows deliberately share coordinates across different slot selections.
    // Original game Y groups geometry; only the separately captured native icon identifies its slot.
    auto markers=[&]() {
        for(size_t row=0;row<slots.size();++row) hud_art_slot(550.f+(float)row*48+4,slots[row]);
    };
    if(markers_first) markers();
    for(size_t i=slots.size();i>0;--i) {
        const float row=(float)(i-1),gy=550.f+row*48,cy=base_y+row*50*sy+oy;
        touch_hud_piece(TZ_ARTS,730,gy+6,804+ox,cy+6*sy,ox,oy);
        touch_hud_piece(TZ_ARTS,726,gy,800+ox,cy,ox,oy);
    }
    if(!markers_first) markers();
    touch_hud_frame();
    for(size_t row=0;row<slots.size();++row)
        points.push_back(ImVec2(pic.x0+(835+ox)*pic.w/1280,
            pic.y0+(base_y+(float)row*50*sy+3*sy+oy)*pic.h/720));
    return points;
}
static std::vector<ImVec2> hunter_art_unmarked_frame(int count) {
    const float sx=squeeze(),sy=squeeze_y();
    const float ox=g_s.layout.zx[TZ_ARTS]*sx,oy=g_s.layout.zy[TZ_ARTS]*sy;
    const float base_y=cfg.touch_hud?180.f:490.f;
    const Pic pic=picture((float)screen_w,(float)screen_h);
    std::vector<ImVec2> points;
    // Only real HUD pieces: this path must never manufacture native icon metadata.
    for(int row=count-1;row>=0;--row) {
        const float gy=583.f+row*48,cy=base_y+row*48*sy+oy;
        touch_hud_piece(TZ_ARTS,730,gy+6,804+ox,cy+6*sy,ox,oy);
        touch_hud_piece(TZ_ARTS,726,gy,800+ox,cy,ox,oy);
    }
    touch_hud_frame();
    for(int row=0;row<count;++row)
        points.push_back(ImVec2(pic.x0+(835+ox)*pic.w/1280,
            pic.y0+(base_y+row*48*sy+3*sy+oy)*pic.h/720));
    return points;
}
static void hunter_art_tap(ImVec2 point,int slot) {
    const uint64_t *expected=EXPECTED_ART_INPUTS;
    touch_reset(); update();
    event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y);
    const Finger *f=finger(g_s,1); assert(f && f->kind==F_ARTS);
    if(f->part!=slot) fprintf(stderr,"ART SLOT FAILURE: expected slot=%d actual=%d at %.1f,%.1f\n",slot,f->part,point.x,point.y);
    assert(f->part==slot);
    event(SDL_EVENT_FINGER_UP,1,point.x,point.y); const Input v=update(B(BTN_B),true);
    const uint64_t direction=slot>=0?expected[slot]:0;
    assert(v.touch_b==direction && v.b==(direction|B(BTN_B))); no_stick_camera(v);
    touch_reset(); assert(!update().touch_b);
}
static void hunter_art_sparse_slots() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(1280,800)})
        for(int moved:{0,1}) for(int mask=1;mask<8;++mask) for(bool markers_first:{false,true}) {
            fresh((int)size.x,(int)size.y); cfg.touch_hud=moved;
            std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
            g_s.layout.zx[TZ_ARTS]=markers_first?71:-36; g_s.layout.zy[TZ_ARTS]=markers_first?-27:39;
            publish_zones(g_s.layout); update();
            std::vector<int> slots;
            for(int slot:{2,0,1}) if(mask&(1<<slot)) slots.push_back(slot);
            const auto points=hunter_art_frame(slots,markers_first);
            for(size_t row=0;row<slots.size();++row) hunter_art_tap(points[row],slots[row]);
            // A true camera drag or cancellation cannot activate the row it started over.
            const ImVec2 p=points.front(); const float dx=.04f*screen_h;
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            event(SDL_EVENT_FINGER_MOTION,1,p.x+dx,p.y);
            event(SDL_EVENT_FINGER_UP,1,p.x+dx,p.y); assert(!update().touch_b); touch_reset();
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            event(SDL_EVENT_FINGER_CANCELED,1,p.x,p.y); assert(!update().touch_b);
            game_menu_state(true);
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind!=F_ARTS);
            event(SDL_EVENT_FINGER_UP,1,p.x,p.y); assert(!(update().touch_b&(DPAD_UP|DPAD_RIGHT|DPAD_DOWN|B(BTN_ZL))));
            touch_reset(); game_menu_state(false);
            const auto returned=hunter_art_frame(slots,!markers_first);
            for(size_t row=0;row<slots.size();++row) hunter_art_tap(returned[row],slots[row]);
        }
    puts("PASS: each native Art slot, all sparse pairs and all three map to Up/Right/Down independently of visible order, shuffled pieces and marker order at four sizes with MoveHUD on/off and custom offsets; drag/cancel/menu never activate Arts and menu close preserves identity");
}
static void hunter_art_unknown_slots() {
    for(int count:{1,2,3}) for(int mode=0;mode<7;++mode) {
        if(count==1 && mode>=5) continue;
        fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); update();
        if(mode==1) { hud_art_slot(587,-1); hud_art_slot(587,3); hud_art_slot(NAN,1); }
        if(mode==2) hud_art_slot(400,2);
        if(mode==3) { hud_art_slot(587,0); hud_art_slot(587,2); if(count>1) hud_art_slot(635,1); }
        if(mode==4) for(int row=0;row<count;++row) hud_art_slot(587.f+row*48,(row+2)%3);
        if(mode==5) hud_art_slot(587,2);
        if(mode==6) for(int row=0;row<count;++row) hud_art_slot(587.f+row*48,2);
        const auto points=hunter_art_unmarked_frame(count);
#ifndef MHGU_TOUCH_BASELINE_NO_ART_SLOT
        if(mode==4) g_zones.art_slot_ns=SDL_GetTicksNS()-stale_limit()-1;
#endif
        for(int row=0;row<count;++row) hunter_art_tap(points[row],row);
    }
    for(bool complete:{false,true}) for(int tapped:{0,1}) {
        fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); update();
        if(complete) { hud_art_slot(587,2); hud_art_slot(635,0); }
        const auto points=hunter_art_unmarked_frame(2); const ImVec2 p=points[tapped];
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
        const uint64_t selected=complete?(tapped?EXPECTED_ART_INPUTS[0]:EXPECTED_ART_INPUTS[2]):
                                         (tapped?EXPECTED_ART_INPUTS[1]:EXPECTED_ART_INPUTS[0]);
        assert(update().touch_b==selected && g_s.art_row==tapped);
        const uint64_t flash_until=g_s.art_flash;
        auto flash=[&]() {
            assert(g_s.art_row==tapped && g_s.art_flash==flash_until && flash_until>SDL_GetTicksNS());
            const Zones z=zones(); float rows[3][2]; assert(arts_rows(z,screen_w,screen_h,rows)==2);
            const Rect arts=zone_rect(z,TZ_ARTS,screen_w,screen_h); const auto vertices=touch_geometry();
            for(int row=0;row<2;++row) {
                int count=0;
                for(const ImVec2 &v:vertices)
                    if(v.x>arts.x0-3 && v.x<arts.x1+3 && v.y>rows[row][0]-3 && v.y<rows[row][1]+3) ++count;
                assert((count>0)==(row==tapped));
            }
            return vertices;
        };
        const auto original=flash();
        // Metadata changes during the same flash must never move it to another visible row.
        for(int phase=0;phase<3;++phase) {
            hud_art_slot(587,2); if(phase!=1) hud_art_slot(635,0);
            hunter_art_unmarked_frame(2);
            if(phase==2) g_zones.art_slot_ns=SDL_GetTicksNS()-stale_limit()-1;
            same_geometry(original,flash());
        }
    }
    puts("PASS: absent, invalid, distant, conflicting, expired, partial and repeated native slot markers fall back as a whole to visible Up/Right/Down rows without disabling or aliasing any Hunter Art");
}
static void hunter_art_no_marker_input() {
    const uint64_t *expected=EXPECTED_ART_INPUTS;
    const uint64_t directions=DPAD_UP|DPAD_RIGHT|DPAD_DOWN;
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(1280,800)})
        for(int moved:{0,1}) for(int offset:{-1,1}) for(int count:{1,2,3}) for(int held=0;held<4;++held) {
            fresh((int)size.x,(int)size.y); cfg.touch_hud=moved;
            std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
            cfg.touch_show[TB_STICK]=(held&1)!=0; cfg.touch_show[TB_DODGE]=(held&2)!=0;
            g_s.layout.x[C_STICK]=g_s.layout.x[C_DODGE]=.18f;
            g_s.layout.y[C_STICK]=g_s.layout.y[C_DODGE]=.20f;
            g_s.layout.zx[TZ_ARTS]=offset*53.f; g_s.layout.zy[TZ_ARTS]=offset*29.f;
            publish_zones(g_s.layout);
            hunt_camera=hunter_moving=false; aiming=false; g_town_ns=0; g_play_ns=0;
            in_quest=false; local_hunter=0; update(); assert(g_s.menu);
            in_quest=true; local_hunter=0x08004000; drawn=1; update(); assert(g_s.menu);
            assert(!g_build.art_slot_mask && !g_zones.art_slot_mask);
            const auto points=hunter_art_unmarked_frame(count);
            const Input entered=update(B(BTN_R),true);
            assert(!g_s.menu && entered.b==B(BTN_R) && !entered.touch_b); no_stick_camera(entered);
            assert(!g_build.art_slot_mask && !g_zones.art_slot_mask);
            if(held&1) {
                at(SDL_EVENT_FINGER_DOWN,C_STICK,2);
                at(SDL_EVENT_FINGER_MOTION,C_STICK,2,.4f);
                assert(finger(g_s,2)->kind==F_STICK);
            }
            if(held&2) {
                at(SDL_EVENT_FINGER_DOWN,C_DODGE,3);
                assert(finger(g_s,3)->kind==F_BUTTON && finger(g_s,3)->ctl==C_DODGE);
            }
            const uint64_t button=(held&2)?B(BTN_B):0;
            const Input held_input=update(B(BTN_R),true);
            assert(held_input.touch_b==button && held_input.b==(button|B(BTN_R)));
            assert((held_input.lx>0)==((held&1)!=0) && !held_input.ly); no_stick_camera(held_input);
            for(int row=0;row<count;++row) {
                const ImVec2 p=points[row];
                event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
                const Finger *f=finger(g_s,1); assert(f && f->kind==F_ARTS);
                if(f->part!=row) fprintf(stderr,"ART UNMARKED FAILURE: expected row=%d actual=%d rows=%d held=%d MoveHUD=%d size=%dx%d\n",row,f->part,count,held,moved,screen_w,screen_h);
                assert(f->part==row);
                const Input down=update(B(BTN_R),true);
                assert(down.touch_b==button && down.b==(button|B(BTN_R)) && down.lx==held_input.lx);
                no_stick_camera(down);
                event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
                const Input up=update(B(BTN_R),true);
                assert(up.touch_b==(button|expected[row]) && up.b==(button|expected[row]|B(BTN_R)));
                assert(up.lx==held_input.lx && !up.ly); no_stick_camera(up);
                if(held&1) assert(finger(g_s,2)->kind==F_STICK);
                if(held&2) assert(finger(g_s,3)->kind==F_BUTTON && finger(g_s,3)->ctl==C_DODGE);
                const Input physical=update_axes(B(BTN_R)|B(BTN_LEFT),0,0,1234,-2345);
                assert(physical.touch_b==(button|expected[row]) && physical.b==(button|expected[row]|B(BTN_R)|B(BTN_LEFT)));
                assert(physical.rx==1234 && physical.ry==-2345 && !physical.dx && !physical.dy);
                assert(pulses(expected[row])==1);
                for(Pulse &pulse:g_s.pulse) if(pulse.buttons==expected[row] && pulse.until>SDL_GetTicksNS()) {
                    assert(!pulse.dpad && pulse.until-pulse.from==120*MS);
                    // Advance just this pulse past expiry without releasing either concurrent touch owner.
                    pulse.until=SDL_GetTicksNS()-1; pulse.from=pulse.until-120*MS;
                }
                const Input expired=update(B(BTN_R),true);
                assert(expired.touch_b==button && expired.b==(button|B(BTN_R)) && expired.lx==held_input.lx);
                no_stick_camera(expired);
            }
            const ImVec2 p=points.front();
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            event(SDL_EVENT_FINGER_MOTION,1,p.x+.04f*screen_h,p.y);
            event(SDL_EVENT_FINGER_UP,1,p.x+.04f*screen_h,p.y);
            assert(update().touch_b==button);
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            event(SDL_EVENT_FINGER_CANCELED,1,p.x,p.y); assert(update().touch_b==button);
            if(held&1) at(SDL_EVENT_FINGER_CANCELED,C_STICK,2);
            if(held&2) at(SDL_EVENT_FINGER_CANCELED,C_DODGE,3);
            assert(!update().touch_b);
            update(B(BTN_PLUS),true); touch_hud_menu(); touch_hud_frame(); update(); assert(g_s.menu);
            test_menu.open=true; test_menu.cols=test_menu.cells=1;
            test_menu.x=p.x/screen_w*1280-20; test_menu.y=p.y/screen_h*720-15;
            test_menu.w=40; test_menu.h=30;
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_GAME_MENU);
            event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
            assert(test_menu.picks==1 && !(update().touch_b&(directions|B(BTN_ZL))));
            assert(!g_build.art_slot_mask && !g_zones.art_slot_mask);
        }
    puts("PASS: marker-free HUD pieces enable all 1/2/3 visible Hunter Art rows on first quest entry at four sizes with MoveHUD on/off and custom offsets; real SDL taps send Type 1 Up/Right/Down for 120 ms while stick/button/physical input survives, and explicit menus, drags and cancellation cannot activate Arts");
}
static void hunter_art_template_offsets() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(1280,800)}) for(int moved:{0,1}) {
        fresh((int)size.x,(int)size.y); cfg.touch_hud=moved;
        std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); update();
        g_s.layout.zx[TZ_ARTS]=53; g_s.layout.zy[TZ_ARTS]=-31; publish_zones(g_s.layout);
        const std::vector<int> slots={2,0}; auto points=hunter_art_frame(slots,true);
        for(size_t row=0;row<slots.size();++row) hunter_art_tap(points[row],slots[row]);
        assert(save_layout_file(g_s.layout,COMMON_PATH));
        touch_edit_begin(); update(); drag_editor_hud(TZ_ARTS,-21,17); editor_click(E_SAVE); update();
        const Layout saved=g_s.layout; assert(touch_template_save(39,"Sparse Art offsets"));
        g_s.layout.zx[TZ_ARTS]=0; g_s.layout.zy[TZ_ARTS]=0; publish_zones(g_s.layout);
        assert(touch_template_use(39)); same_layout(g_s.layout,saved);
        points=hunter_art_frame(slots,false);
        for(size_t row=0;row<slots.size();++row) hunter_art_tap(points[row],slots[row]);
        Layout disk; assert(load_layout_file(&disk,COMMON_PATH)); same_layout(disk,saved);
        touch_reset(); g_s={}; update(); same_layout(g_s.layout,saved);
        points=hunter_art_frame(slots,true);
        for(size_t row=0;row<slots.size();++row) hunter_art_tap(points[row],slots[row]);
    }
    puts("PASS: sparse native Art identities survive actual editor dragging, Save, template 40 apply and layout reload with MoveHUD on/off at four sizes; stored offsets retain their values");
}
static void hunter_art_hidden_controls() {
    const uint64_t *expected=EXPECTED_ART_INPUTS;
    for(int mode:{TOUCH_OFF,TOUCH_AUTO}) for(int hide_at:{0,1,2}) for(int slot:{0,1,2}) {
        fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); update();
        const auto points=hunter_art_frame({2,0,1},true);
        const ImVec2 p=points[(slot+1)%3];
        if(hide_at==0) { cfg.touch_controls=mode; update(0,true); }
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
        assert(finger(g_s,1)->kind==F_ARTS && finger(g_s,1)->part==slot);
        if(hide_at==1) { cfg.touch_controls=mode; update(0,true); }
        event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
        if(hide_at==2) cfg.touch_controls=mode;
        const Input v=update_axes(B(BTN_R),1234,-2345,3456,-4567);
        assert(!g_visible && v.touch_b==expected[slot] && v.b==(B(BTN_R)|expected[slot]));
        assert(v.lx==1234 && v.ly==-2345 && v.rx==3456 && v.ry==-4567 && !v.dx && !v.dy);
        assert(pulses(expected[slot])==1);
        touch_reset(); assert(!update(0,true).touch_b);
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
        event(SDL_EVENT_FINGER_MOTION,1,p.x+.04f*screen_h,p.y);
        event(SDL_EVENT_FINGER_UP,1,p.x+.04f*screen_h,p.y);
        const Input drag=update(0,true); assert(!drag.touch_b && drag.dx>0 && !drag.dy);
    }
    // A visible button over a bar keeps ownership; hiding that button exposes the native bar.
    fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); cfg.touch_show[TB_ATTACKS]=1;
    const ImVec2 p=hunter_art_frame({2},true).front();
    g_s.layout.x[C_HEAVY]=(screen_w-p.x)/screen_h; g_s.layout.y[C_HEAVY]=1-p.y/screen_h;
    update(); event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
    assert(finger(g_s,1)->kind==F_BUTTON && finger(g_s,1)->ctl==C_HEAVY);
    assert(update().touch_b==B(BTN_A));
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
    cfg.touch_controls=TOUCH_OFF; assert(!update().touch_b);
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_ARTS);
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y); assert(update().touch_b==EXPECTED_ART_INPUTS[2]);
    puts("PASS: native Art taps survive Off/Auto controls hidden before, during or after a tap, preserve physical buttons and axes, and allow camera drags; visible virtual buttons keep priority and their quick-tap pulses stop when hidden");
}
static void hunter_art_cancel_context() {
    const uint64_t art_mask=B(BTN_ZL)|DPAD_UP|DPAD_RIGHT|DPAD_DOWN;
    for(bool hidden:{false,true}) for(bool released:{false,true}) for(int stop=0;stop<8;++stop) {
        fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
        cfg.touch_controls=hidden?TOUCH_OFF:TOUCH_ON; update();
        const ImVec2 p=hunter_art_frame({2},true).front();
        if(stop==7) assert(touch_template_save(39,"Art cancellation"));
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); assert(finger(g_s,1)->kind==F_ARTS);
        if(released) {
            event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
            assert(update().touch_b==EXPECTED_ART_INPUTS[2]);
        }
        switch(stop) {
        case 0: settings_open=true; break;
        case 1: game_menu_state(true); break;
        case 2: break;  // typing is supplied to the input update below
        case 3: in_quest=false; break;
        case 4: hud_timestamp(g_zones.ns,TZ_ARTS)=SDL_GetTicksNS()-stale_limit()-1; break;
        case 5: touch_edit_begin(); break;
        case 6: touch_reset(); break;
        case 7: assert(touch_template_use(39)); break;
        }
        const Input blocked=update(0,false,stop==2);
        assert(!(blocked.touch_b&art_mask)); no_stick_camera(blocked);
        if(!released) event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
        assert(!(update(0,false,stop==2).touch_b&art_mask));
        // Returning to live HUD input must not revive the old touch or its pending press.
        settings_open=false; g_menu_ns=0; g_window_open=false; in_quest=true; g_s.edit=false;
        hunter_art_frame({2},true); const Input restored=update();
        assert(!(restored.touch_b&art_mask)); no_stick_camera(restored);
    }
    puts("PASS: menus, typing, quest exit, expired HUD, editor, reset and template replacement cancel held or released Art taps with controls shown or hidden, and returning to gameplay cannot revive those presses");
}
static void hunter_art_control_types() {
    // Independently specified native masks; the production option reader is covered by test_ranged_input.c.
    const uint64_t native[4][2][3]={
        {{0x2000,0x4000,0x8000},{0x2000,0x4000,0x8000}},
        {{0x83,0x100,0x200},{0x203,0x40,0x80}},
        {{0x104,0x101,0x102},{0x44,0x41,0x42}},
        {{0x200000,0x400000,0x800000},{0x200000,0x400000,0x800000}},
    };
    auto unchanged=[](uint64_t buttons) {
        const Input v=update_axes(buttons,1234,-2345,32767,-32767);
        assert(v.b==buttons && !v.touch_b && v.lx==1234 && v.ly==-2345 && v.rx==32767 && v.ry==-32767);
        assert(!v.dx && !v.dy && !touch_virtual_right_stick_active());
    };
    for(int type=0;type<4;++type) for(int shoulders=0;shoulders<2;++shoulders)
        for(int directions=0;directions<2;++directions) for(bool hidden:{false,true}) for(int slot=0;slot<3;++slot) {
            fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
            cfg.touch_controls=hidden?TOUCH_OFF:TOUCH_ON;
            const int mapped=directions && (type==0 || type==3)?3-type:type;
            std::copy(native[mapped][shoulders],native[mapped][shoulders]+3,art_inputs);
            update();
            // Full native button/stick input remains available without a HUD tap in every configuration.
            for(int bit=0;bit<24;++bit) unchanged(1ull<<bit);
            unchanged(native[mapped][shoulders][slot]);
            const auto points=hunter_art_frame({2,0,1},true);
            const ImVec2 p=points[(slot+1)%3];
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            assert(finger(g_s,1)->kind==F_ARTS && finger(g_s,1)->part==slot);
            unchanged(B(BTN_R)|B(BTN_LEFT));
            event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
            const uint64_t expected=native[mapped][shoulders][slot];
            const bool stick=mapped==3;
            const int ax=stick && slot==1?32767:0,ay=stick?(slot==0?32767:slot==2?-32767:0):0;
            const Input tap=update();
            assert(tap.b==expected && tap.touch_b==expected && !tap.lx && !tap.ly);
            assert(tap.rx==ax && tap.ry==ay && !tap.dx && !tap.dy);
            assert(!touch_virtual_right_stick_active()); // Art input must not suppress native D-pad camera events.
            // A second finger still looks around while an Art pulse (including right-stick Arts) is active.
            event(SDL_EVENT_FINGER_DOWN,2,640,100);
            assert(finger(g_s,2)->kind==F_CAMERA);
            event(SDL_EVENT_FINGER_MOTION,2,676,73);
            const Input mixed=update_axes(B(BTN_R)|B(BTN_LEFT),1234,-2345,3456,-4567);
            assert(mixed.b==(expected|B(BTN_R)|B(BTN_LEFT)) && mixed.touch_b==expected);
            assert(mixed.lx==1234 && mixed.ly==-2345 && mixed.rx==3456+ax && mixed.ry==-4567+ay);
            assert(fabsf(mixed.dx-36*CAMERA_COUNTS/screen_h)<.01f && fabsf(mixed.dy+27*CAMERA_COUNTS/screen_h)<.01f);
            assert(!touch_virtual_right_stick_active());
            event(SDL_EVENT_FINGER_UP,2,676,73);
            const Input still=update();
            assert(still.touch_b==expected && still.rx==ax && still.ry==ay && !still.dx && !still.dy);
            assert(pulses(expected)==1);
            for(Pulse &pulse:g_s.pulse) if(pulse.art && pulse.until>SDL_GetTicksNS()) {
                assert(pulse.buttons==expected && !pulse.dpad && pulse.until-pulse.from==120*MS);
                pulse.until=SDL_GetTicksNS()-1;
            }
            unchanged(expected|B(BTN_R)|B(BTN_LEFT));
            // Starting a swipe on an Art row becomes camera look, never another Art or right-stick command.
            event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
            event(SDL_EVENT_FINGER_MOTION,1,p.x+36,p.y-27);
            assert(finger(g_s,1)->kind==F_CAMERA);
            const Input drag=update();
            assert(!drag.b && !drag.touch_b && !drag.lx && !drag.ly && !drag.rx && !drag.ry);
            assert(drag.dx>0 && drag.dy<0 && !touch_virtual_right_stick_active());
            event(SDL_EVENT_FINGER_UP,1,p.x+36,p.y-27);
            unchanged(B(BTN_R));
        }
    // Every type cancels both pending taps and active pulses, including real right-stick axes.
    for(int type=0;type<4;++type) for(bool hidden:{false,true}) for(bool released:{false,true}) for(int stop=0;stop<7;++stop) {
        fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
        cfg.touch_controls=hidden?TOUCH_OFF:TOUCH_ON;
        std::copy(native[type][1],native[type][1]+3,art_inputs); update();
        const ImVec2 p=hunter_art_frame({2},true).front();
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
        assert(finger(g_s,1)->kind==F_ARTS && finger(g_s,1)->part==2);
        if(released) { event(SDL_EVENT_FINGER_UP,1,p.x,p.y); assert(update().touch_b==native[type][1][2]); }
        switch(stop) {
        case 0: settings_open=true; break;
        case 1: game_menu_state(true); break;
        case 2: break; // typing is supplied below
        case 3: in_quest=false; break;
        case 4: hud_timestamp(g_zones.ns,TZ_ARTS)=SDL_GetTicksNS()-stale_limit()-1; break;
        case 5: touch_edit_begin(); break;
        case 6: touch_reset(); break;
        }
        const Input blocked=update(B(BTN_R),true,stop==2);
        assert(blocked.b==B(BTN_R) && !blocked.touch_b); neutral_sticks(blocked);
        assert(!touch_virtual_right_stick_active());
        if(!released) event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
        settings_open=false; g_menu_ns=0; g_window_open=false; in_quest=true; g_s.edit=false;
        hunter_art_frame({2},true); unchanged(B(BTN_R));
    }
    fresh(); std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
    std::fill(std::begin(art_inputs),std::end(art_inputs),0); update();
    const ImVec2 p=hunter_art_frame({0},true).front();
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_UP,1,p.x,p.y);
    unchanged(B(BTN_R)); assert(!g_s.art_flash);
    puts("PASS: all four native control types, shoulder/direction swaps and sparse slots activate by one HUD tap with controls shown/hidden; swipes remain camera look during Art pulses and on Art rows, native inputs survive, right-stick axes expire/cancel, and unknown options emit no input");
}
static void editor_hud_priority_and_multitouch() {
    fresh(); assert(save_layout_file(g_s.layout,COMMON_PATH)); update(); touch_edit_begin(); update();
    const Geo button=place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h);
    const float sx=squeeze(),sy=squeeze_y();
    const ImVec2 point(button.c.x+1.25f*button.r,button.c.y);
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
    Rect panel,buttons[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,buttons);
    const float x=(buttons[E_SMALLER].x0+buttons[E_SMALLER].x1)*.5f,y=(buttons[E_SMALLER].y0+buttons[E_SMALLER].y1)*.5f;
    touch_hud_piece(TZ_TARGET,1200,55,x,y,0,0); touch_hud_frame();
    event(SDL_EVENT_FINGER_DOWN,1,x,y); assert(finger(g_s,1)->kind==F_MENU);
    event(SDL_EVENT_FINGER_UP,1,x,y); editor_click(E_CANCEL); update();
    puts("PASS: visible button graphics stay draggable over HUD, HUD wins over expanded button hit slop, toolbar wins over HUD, and extra fingers cannot pinch a HUD or alter button arrays");
}
static void ammo_repeat() {
    fresh(); ranged=WEAPON_BOWGUN; drawn=1; update();
    const ImVec2 p=hud(TZ_ITEM,640,620);
    event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y); event(SDL_EVENT_FINGER_MOTION,1,p.x,p.y-120);
    assert(pulses(B(BTN_X))==1 && update().b==B(BTN_L));
    g_s.ammo_pulse.from=SDL_GetTicksNS()+2000*MS;
    g_s.ammo_pulse.until=g_s.ammo_pulse.from+80*MS;
    g_s.ammo_ready=g_s.ammo_pulse.from+170*MS; g_s.ammo_hold=g_s.ammo_pulse.from+320*MS;
    const uint64_t ready=g_s.ammo_ready;
    event(SDL_EVENT_FINGER_MOTION,1,p.x,p.y+120);
    assert(pulses(B(BTN_B))==1 && !pulses(B(BTN_X)));
    for(int i=0;i<100;++i) {
        event(SDL_EVENT_FINGER_MOTION,1,p.x,p.y+(i%2?120:-120));
        assert(pulses(B(BTN_X))+pulses(B(BTN_B))==1 && g_s.ammo_ready==ready);
    }
    for(const Pulse &q:g_s.pulse) assert(!q.until);
    g_s.ammo_pulse={}; g_s.ammo_ready=0;
    update(); assert(!pulses(B(BTN_X)) && !pulses(B(BTN_B)));
    event(SDL_EVENT_FINGER_UP,1,p.x,p.y+120); const int pending=pulses(B(BTN_X))+pulses(B(BTN_B));
    for(int i=0;i<20;++i) update(); assert(pulses(B(BTN_X))+pulses(B(BTN_B))<=pending);
    touch_reset(); assert(!update().b && !g_s.ammo_hold && !g_s.ammo_ready);
    puts("PASS: common-layout HUD ammo slides reverse one bounded pending step without generic pulse growth; motion stops blind repeat, lift stops queueing and reset releases touch-owned L/ammo");
}

static void arrow_gesture_ownership() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)})
    for(int weapon:{0,4,6,10}) for(bool ammo:{false,true}) for(int direction:{-1,1}) for(int contact=0;contact<3;++contact) {
        if(ammo && !weapon) continue;
        fresh((int)size.x,(int)size.y); weapon_override=weapon; ranged=weapon==10?WEAPON_BOW:weapon?WEAPON_BOWGUN:0;
        local_hunter=0x08004000; drawn=1;
        std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0);
        cfg.touch_show[TB_ATTACKS]=cfg.touch_show[TB_AMMO]=1; cfg.touch_show[TB_ITEM_ARROWS]=!ammo; update();
        const int ctl=direction>0?C_AMMO_UP:C_AMMO_DOWN;
        g_s.layout.scale[ctl]=contact==1?.65f:contact==2?1.5f:1;
        Geo arrow;
        if(ammo) arrow=place(g_s.layout,ctl,(float)screen_w,(float)screen_h);
        else {
            hud(TZ_ITEM,640,400);
            arrow=item_arrow(zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h),direction,(float)screen_h);
            // The item HUD arrow must also win over an editable ammo arrow beneath it.
            g_s.layout.x[ctl]=(screen_w-arrow.c.x)/screen_h; g_s.layout.y[ctl]=1-arrow.c.y/screen_h;
        }
        ImVec2 point=arrow.c;
        if(contact==1) point.y+=arrow.r*1.1f; // Arrow hit slop over an ordinary button's visible face.
        const float light_r=DEFS[C_LIGHT].r*screen_h;
        g_s.layout.x[C_LIGHT]=(screen_w-point.x-(contact==2?light_r*1.1f:0))/screen_h;
        g_s.layout.y[C_LIGHT]=1-point.y/screen_h;
        auto owner=[&]() {
            const Finger *f=finger(g_s,1);
            const bool ok=f && (ammo?(f->kind==F_BUTTON && f->ctl==ctl && f->part==2):(f->kind==F_ITEM && f->part==1));
            if(!ok) fprintf(stderr,"ARROW OWNER FAILURE: %dx%d weapon=%d ammo=%d dir=%d contact=%d kind=%d ctl=%d part=%d\n",screen_w,screen_h,weapon,ammo,direction,contact,f?f->kind:-1,f?f->ctl:-1,f?f->part:-1);
            assert(ok);
        };
        auto selection_only=[&]() {
            const uint64_t physical=B(BTN_R)|B(BTN_ZR); const Input v=update(physical,true);
            const uint64_t allowed=ammo?(B(BTN_L)|B(BTN_X)|B(BTN_B)):B(BTN_L);
            if(v.dx || v.dy || v.rx || v.ry || (v.touch_b&~allowed) || bow_drag.begins)
                fprintf(stderr,"ARROW INPUT FAILURE: %dx%d weapon=%d ammo=%d dir=%d contact=%d touch=%llu camera=%f,%f bow=%d\n",screen_w,screen_h,weapon,ammo,direction,contact,(unsigned long long)v.touch_b,v.dx,v.dy,bow_drag.begins);
            assert(!(v.touch_b&~allowed) && v.b==(v.touch_b|physical) && !bow_drag.begins && !bow_drag.adds);
            no_stick_camera(v); return v;
        };
        event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y); owner();
        assert(selection_only().touch_b==B(BTN_L));
        if(ammo) assert(pulses(B(direction>0?BTN_X:BTN_B))==1 && item_steps.empty());
        else assert(item_steps==std::vector<int>{direction} && !pulses(B(BTN_X)) && !pulses(B(BTN_B)));
        event(SDL_EVENT_FINGER_MOTION,1,point.x+screen_h*.2f,point.y-screen_h*.15f); owner(); selection_only();
        event(SDL_EVENT_FINGER_MOTION,1,point.x-screen_h*.2f,point.y+screen_h*.15f); owner(); selection_only();
        // A separate empty-space finger still owns the ordinary camera while the arrow finger moves.
        const ImVec2 camera(screen_w*.5f,screen_h*.25f);
        event(SDL_EVENT_FINGER_DOWN,2,camera.x,camera.y); assert(finger(g_s,2)->kind==F_CAMERA);
        event(SDL_EVENT_FINGER_MOTION,2,camera.x+screen_h*.07f,camera.y-screen_h*.04f);
        event(SDL_EVENT_FINGER_MOTION,1,point.x+screen_h*.3f,point.y-screen_h*.2f);
        Input v=update(); camera_delta(v,screen_h*.07f,-screen_h*.04f); assert(!v.rx && !v.ry && !bow_drag.begins);
        event(SDL_EVENT_FINGER_CANCELED,2,camera.x+10000,camera.y-10000); selection_only();
        event(SDL_EVENT_FINGER_UP,1,point.x+10000,point.y-10000); selection_only(); assert(!finger(g_s,1));
        for(const Uint32 release:{SDL_EVENT_FINGER_UP,SDL_EVENT_FINGER_CANCELED}) {
            touch_reset(); update(); item_steps.clear();
            if(!ammo) hud(TZ_ITEM,640,400);
            event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y); owner(); selection_only();
            const size_t accepted=item_steps.size();
            event(release,1,point.x-10000,point.y+10000); selection_only(); assert(!finger(g_s,1));
            if(release==SDL_EVENT_FINGER_CANCELED) {
                assert(item_steps.size()==accepted && !g_s.ammo_pulse.until && !touch_item_pending);
                event(SDL_EVENT_FINGER_UP,1,point.x+10000,point.y-10000); selection_only();
                assert(item_steps.size()==accepted && !g_s.ammo_pulse.until && !touch_item_pending);
            }
        }
        for(const Pulse &p:g_s.pulse) assert(!p.until);
        if(ammo) assert(item_steps.empty());
    }
    puts("PASS: both item and ammo arrows own hits above ordinary button faces/slop, item arrows also win over ammo, and 1.2 arrow slop works at three sizes for melee/HBG/LBG/Bow; drag, distant UP-only and CANCEL never move camera or Bow charge, a separate finger still steers, native selection and physical R/ZR stay independent");
}

static void ammo_visibility_and_editor() {
    for(bool quest:{false,true}) for(int weapon:{0,4,6,10,UNKNOWN_WEAPON}) for(bool shown:{false,true}) for(bool menu:{false,true}) {
        fresh(); in_quest=quest; weapon_override=weapon; ranged=weapon==10?WEAPON_BOW:(weapon==4 || weapon==6)?WEAPON_BOWGUN:0;
        local_hunter=0x08004000; drawn=1; std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),0); cfg.touch_show[TB_AMMO]=shown;
        game_menu_state(menu); const bool eligible=quest && ranged && shown;
        for(int c:{C_AMMO_UP,C_AMMO_DOWN}) {
            assert(in_mode(c,menu,quest)==(eligible && !menu) && in_editor(c,quest,profile)==eligible);
            const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            assert((drawn_vertices(g.c,g.r*.7f)>0)==(eligible && !menu));
            at(SDL_EVENT_FINGER_DOWN,c); const Finger *f=finger(g_s,1);
            assert((f && f->kind==F_BUTTON && f->ctl==c)==(eligible && !menu));
            at(SDL_EVENT_FINGER_CANCELED,c); no_stick_camera(update());
        }
    }
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) for(int c:{C_AMMO_UP,C_AMMO_DOWN}) {
        fresh((int)size.x,(int)size.y); ranged=WEAPON_BOW; weapon_override=10; local_hunter=0x08004000; cfg.touch_show[TB_AMMO]=1;
        enable_universal_combos(); update(); assert(save_layout_file(g_s.layout,COMMON_PATH)); const Layout original=g_s.layout;
        const Geo arrow=place(g_s.layout,c,(float)screen_w,(float)screen_h);
        for(int combo=C_COMBO;combo<C_COMBO+TOUCH_COMBOS;++combo) {
            const Geo g=place(g_s.layout,combo,(float)screen_w,(float)screen_h);
            assert(hypotf(arrow.c.x-g.c.x,arrow.c.y-g.c.y)>1.22f*(arrow.r+g.r));
        }
        for(bool save:{false,true}) {
            touch_edit_begin(); update(); const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            at(SDL_EVENT_FINGER_DOWN,c); assert(g_s.sel==c && finger(g_s,1)->kind==F_EDIT);
            event(SDL_EVENT_FINGER_MOTION,1,g.c.x-screen_h*.10f,g.c.y-screen_h*.08f);
            event(SDL_EVENT_FINGER_UP,1,g.c.x-screen_h*.10f,g.c.y-screen_h*.08f); editor_click(E_BIGGER);
            const Layout draft=g_s.layout;
            assert(fabsf(draft.x[c]-original.x[c]-.10f)<.0001f && fabsf(draft.y[c]-original.y[c]-.08f)<.0001f && draft.scale[c]>1.09f);
            no_stick_camera(update()); assert(!bow_drag.begins && item_steps.empty());
            editor_click(save?E_SAVE:E_CANCEL); update(); same_layout(g_s.layout,save?draft:original);
            Layout disk; assert(load_layout_file(&disk,COMMON_PATH)); same_layout(disk,save?draft:original);
            touch_reset(); g_s={}; update(); same_layout(g_s.layout,save?draft:original);
        }
    }
    puts("PASS: independent ammo arrows draw/hit/edit only for quest HBG/LBG/Bow with ShowAmmo; town, melee, unknown weapon and menus suppress runtime arrows; both defaults clear all six combos, and drag/scale Cancel/Save/restart preserve their independent saved geometry at three sizes");
}

static void arrow_gesture_lifecycle() {
    for(bool ammo:{false,true}) for(int mode=0;mode<11;++mode) {
        if(!ammo && mode==6) continue;
        fresh(); ranged=WEAPON_BOW; weapon_override=10; local_hunter=0x08004000; drawn=1;
        cfg.touch_show[TB_AMMO]=cfg.touch_show[TB_ITEM_ARROWS]=1; update();
        Geo arrow;
        if(ammo) arrow=place(g_s.layout,C_AMMO_UP,(float)screen_w,(float)screen_h);
        else { hud(TZ_ITEM,640,400); arrow=item_arrow(zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h),1,(float)screen_h); }
        event(SDL_EVENT_FINGER_DOWN,1,arrow.c.x,arrow.c.y); assert(item_gesture(*finger(g_s,1)));
        event(SDL_EVENT_FINGER_MOTION,1,arrow.c.x+70,arrow.c.y-70); no_stick_camera(update());
        const size_t accepted=item_steps.size();
        switch(mode) {
            case 0: game_menu_state(true); break;
            case 1: settings_open=true; break;
            case 2: cfg.touch_controls=TOUCH_OFF; break;
            case 3: cfg.touch_show[ammo?TB_AMMO:TB_ITEM_ARROWS]=0; break;
            case 4: touch_edit_begin(); break;
            case 5: in_quest=false; break;
            case 6: ranged=0; weapon_override=0; break;
            case 7: touch_reset(); break;
            case 8: assert(touch_template_save(19,"Arrow reset") && touch_template_use(19)); break;
            case 9: break;
            case 10: settings_open=true; event(SDL_EVENT_FINGER_UP,1,arrow.c.x+10000,arrow.c.y-10000); break;
        }
        auto stopped=[&]() {
            const uint64_t physical=B(BTN_L)|B(BTN_X)|B(BTN_B)|B(BTN_R);
            const Input v=update(physical,true,mode==9);
            if(finger(g_s,1) || v.touch_b || touch_item_pending || g_s.ammo_pulse.until)
                fprintf(stderr,"ARROW LIFECYCLE FAILURE: ammo=%d mode=%d finger=%d touch=%llu item=%d ammo_until=%llu\n",ammo,mode,finger(g_s,1)!=nullptr,(unsigned long long)v.touch_b,touch_item_pending,(unsigned long long)g_s.ammo_pulse.until);
            assert(!finger(g_s,1) && !v.touch_b && v.b==physical && !touch_item_pending && !g_s.ammo_pulse.until && !g_s.ammo_hold && !g_s.ammo_ready);
            no_stick_camera(v); assert(!bow_drag.begins && item_steps.size()==accepted);
        };
        stopped(); event(SDL_EVENT_FINGER_UP,1,arrow.c.x-10000,arrow.c.y+10000); stopped();
        settings_open=false; cfg.touch_controls=TOUCH_ON; cfg.touch_show[TB_AMMO]=cfg.touch_show[TB_ITEM_ARROWS]=1;
        if(g_s.edit) { editor_click(E_CANCEL); update(); } if(g_s.menu) game_menu_state(false);
        in_quest=true; ranged=WEAPON_BOW; weapon_override=10; stopped();
    }
    fresh(); cfg.touch_show[TB_ITEM_ARROWS]=1; const ImVec2 item=hud(TZ_ITEM,640,400);
    event(SDL_EVENT_FINGER_DOWN,1,item.x,item.y); event(SDL_EVENT_FINGER_MOTION,1,item.x+100,item.y);
    cfg.touch_show[TB_ITEM_ARROWS]=0; const Input item_only=update();
    assert(finger(g_s,1) && finger(g_s,1)->kind==F_ITEM && item_only.touch_b==B(BTN_L) && touch_item_pending==1);
    no_stick_camera(item_only); event(SDL_EVENT_FINGER_CANCELED,1,item.x+100,item.y); assert(!update().touch_b && !touch_item_pending);
    puts("PASS: item/ammo arrow ownership and pending selection clear on menus, overlays including event-only UP, Off, hidden arrows, editor, scene changes, reset, templates and typing; ammo also clears on weapon change, and old UP/return never revives input while physical L/X/B/R survive");
}

static void arrow_release_pending_and_overlap() {
    int failures=0;
    auto check=[&](bool ok,const char *label,int weapon,int direction,int mode) {
        if(!ok) { ++failures; fprintf(stderr,"ARROW RELEASE FAILURE: %s weapon=%d direction=%d mode=%d\n",label,weapon,direction,mode); }
    };
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) for(float scale:{.7f,1.4f}) {
        fresh((int)size.x,(int)size.y); ranged=WEAPON_BOW; weapon_override=10; local_hunter=0x08004000;
        cfg.touch_show[TB_AMMO]=1; update();
        g_s.layout.x[C_AMMO_DOWN]=g_s.layout.x[C_AMMO_UP]; g_s.layout.y[C_AMMO_DOWN]=g_s.layout.y[C_AMMO_UP];
        g_s.layout.scale[C_AMMO_UP]=g_s.layout.scale[C_AMMO_DOWN]=scale;
        at(SDL_EVENT_FINGER_DOWN,C_AMMO_DOWN); const Finger *f=finger(g_s,1);
        check(f && f->kind==F_BUTTON && f->ctl==C_AMMO_DOWN && pulses(B(BTN_B))==1 && !pulses(B(BTN_X)),
              "last-drawn overlapping DOWN must win",10,-1,(int)(scale*screen_h));
        no_stick_camera(update()); at(SDL_EVENT_FINGER_CANCELED,C_AMMO_DOWN); assert(!update().touch_b);
    }
    for(int weapon:{4,6,10}) for(int direction:{-1,1}) for(int change=0;change<3;++change) {
        fresh(); ranged=weapon==10?WEAPON_BOW:WEAPON_BOWGUN; weapon_override=weapon; local_hunter=0x08004000;
        cfg.touch_show[TB_AMMO]=1; update(); const int ctl=direction>0?C_AMMO_UP:C_AMMO_DOWN;
        at(SDL_EVENT_FINGER_DOWN,ctl); at(SDL_EVENT_FINGER_UP,ctl);
        assert(!finger(g_s,1) && g_s.ammo_pulse.from>SDL_GetTicksNS() && update().touch_b==B(BTN_L));
        if(change==0) cfg.touch_show[TB_AMMO]=0;
        else { ranged=0; weapon_override=change==1?0:UNKNOWN_WEAPON; }
        const uint64_t physical=B(BTN_L)|B(BTN_X)|B(BTN_B)|B(BTN_R);
        Input v=update(physical,true);
        check(!v.touch_b && v.b==physical && !g_s.ammo_pulse.until && !g_s.ammo_hold && !g_s.ammo_ready,
              "released ammo tail must cancel",weapon,direction,change); no_stick_camera(v);
        cfg.touch_show[TB_AMMO]=1; ranged=weapon==10?WEAPON_BOW:WEAPON_BOWGUN; weapon_override=weapon;
        v=update(physical,true); check(!v.touch_b && v.b==physical,"restoring arrows cannot revive tail",weapon,direction,change);
        no_stick_camera(v); assert(item_steps.empty() && !bow_drag.begins);
    }
    // ShowAmmo governs the added buttons; native vertical item-bar selection remains available.
    for(int weapon:{4,6,10}) for(int direction:{-1,1}) for(bool initially_hidden:{false,true}) {
        fresh(); ranged=weapon==10?WEAPON_BOW:WEAPON_BOWGUN; weapon_override=weapon; local_hunter=0x08004000;
        cfg.touch_show[TB_AMMO]=!initially_hidden; update(); const ImVec2 item=hud(TZ_ITEM,640,620);
        event(SDL_EVENT_FINGER_DOWN,1,item.x,item.y); event(SDL_EVENT_FINGER_UP,1,item.x,item.y-direction*120);
        assert(!finger(g_s,1) && pulses(B(direction>0?BTN_X:BTN_B))==1);
        const uint64_t until=g_s.ammo_pulse.until,hold=g_s.ammo_hold; cfg.touch_show[TB_AMMO]=0;
        Input v=update(B(BTN_R),true);
        check(v.touch_b==B(BTN_L) && v.b==(B(BTN_L)|B(BTN_R)) && g_s.ammo_pulse.until==until && g_s.ammo_hold==hold,
              "hidden ammo buttons must preserve HUD pending selection",weapon,direction,initially_hidden); no_stick_camera(v);
        g_s.ammo_pulse.from=0; g_s.ammo_pulse.until=SDL_GetTicksNS()+80*MS;
        v=update(); check(v.touch_b==(B(BTN_L)|B(direction>0?BTN_X:BTN_B)),"HUD pending selection must still fire",weapon,direction,initially_hidden);
        no_stick_camera(v); assert(item_steps.empty() && !bow_drag.begins);
    }
    for(int direction:{-1,1}) for(bool arrow_source:{false,true}) {
        fresh(); cfg.touch_show[TB_ITEM_ARROWS]=1; const ImVec2 item=hud(TZ_ITEM,640,400);
        const Geo arrow=item_arrow(zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h),direction,(float)screen_h);
        const ImVec2 start=arrow_source?arrow.c:item;
        event(SDL_EVENT_FINGER_DOWN,1,start.x,start.y);
        event(SDL_EVENT_FINGER_UP,1,start.x+(arrow_source?0:direction*120),start.y);
        assert(!finger(g_s,1) && touch_item_pending==direction);
        cfg.touch_show[TB_ITEM_ARROWS]=0; Input v=update(B(BTN_R),true);
        check(touch_item_pending==(arrow_source?0:direction) && !v.touch_b && v.b==B(BTN_R),
              "released item request must respect arrow/HUD origin",0,direction,arrow_source); no_stick_camera(v);
        event(SDL_EVENT_FINGER_DOWN,2,item.x,item.y); event(SDL_EVENT_FINGER_UP,2,item.x+120,item.y);
        v=update(); check(touch_item_pending==1 && !v.touch_b,"new horizontal HUD request must survive hidden arrows",0,direction,arrow_source);
        no_stick_camera(v);
    }
    // A newer native HUD gesture replaces an earlier arrow's pending ammo source.
    fresh(); ranged=WEAPON_BOW; weapon_override=10; local_hunter=0x08004000; cfg.touch_show[TB_AMMO]=1; update();
    at(SDL_EVENT_FINGER_DOWN,C_AMMO_UP); at(SDL_EVENT_FINGER_UP,C_AMMO_UP);
    const ImVec2 item=hud(TZ_ITEM,640,620);
    event(SDL_EVENT_FINGER_DOWN,2,item.x,item.y); event(SDL_EVENT_FINGER_UP,2,item.x,item.y+120);
    cfg.touch_show[TB_AMMO]=0; const Input v=update();
    check(v.touch_b==B(BTN_L) && pulses(B(BTN_B))==1 && !pulses(B(BTN_X)),"new HUD ammo source replaces prior arrow source",10,-1,0);
    no_stick_camera(v);
    for(bool ammo:{false,true}) {
        fresh(); ranged=WEAPON_BOW; weapon_override=10; local_hunter=0x08004000;
        cfg.touch_show[TB_AMMO]=cfg.touch_show[TB_ITEM_ARROWS]=1; update(); const ImVec2 item=hud(TZ_ITEM,640,620);
        const Geo arrow=ammo?place(g_s.layout,C_AMMO_UP,(float)screen_w,(float)screen_h):
            item_arrow(zone_rect(zones(),TZ_ITEM,(float)screen_w,(float)screen_h),1,(float)screen_h);
        event(SDL_EVENT_FINGER_DOWN,1,arrow.c.x,arrow.c.y);
        event(SDL_EVENT_FINGER_DOWN,2,item.x,item.y);
        event(SDL_EVENT_FINGER_MOTION,2,item.x-(ammo?0:120),item.y+(ammo?120:0));
        assert(finger(g_s,1) && finger(g_s,2) && finger(g_s,2)->kind==F_ITEM);
        cfg.touch_show[ammo?TB_AMMO:TB_ITEM_ARROWS]=0; const Input mixed=update();
        const Finger *hud_finger=finger(g_s,2);
        check(!finger(g_s,1) && hud_finger && hud_finger->kind==F_ITEM && mixed.touch_b==B(BTN_L) &&
              (ammo?(pulses(B(BTN_B))==1 && !pulses(B(BTN_X))):touch_item_pending==-1),
              "hiding one arrow must preserve another finger's HUD selection",10,-1,ammo);
        no_stick_camera(mixed); event(SDL_EVENT_FINGER_CANCELED,2,item.x-120,item.y+120);
        assert(!update().touch_b && !touch_item_pending && !g_s.ammo_pulse.until);
    }
    assert(!failures);
    puts("PASS: overlapping ammo buttons hit the last-drawn DOWN; released arrow-owned ammo/item requests cancel when hidden or weapon-invalid and never revive, while hidden-arrow settings preserve native vertical/horizontal HUD selection and newer HUD ownership");
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
        else if(mode==5) assert(touch_template_save(19,"Input reset") && touch_template_use(19));
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
static void dedicated_lock_button() {
    fresh(); lock_on=true;
    assert(in_mode(C_LOCK,false,true) && !in_mode(C_LOCK,false,false));
    assert(in_editor(C_LOCK,false,profile));
    at(SDL_EVENT_FINGER_DOWN,C_LOCK);
    assert(finger(g_s,1)->kind==F_BUTTON && finger(g_s,1)->ctl==C_LOCK);
    at(SDL_EVENT_FINGER_UP,C_LOCK);
    assert(g_s.lock && target_cycles==0);
    touch_lock_buttons(0,false); assert(touch_lock_on());
    at(SDL_EVENT_FINGER_DOWN,C_LOCK); at(SDL_EVENT_FINGER_UP,C_LOCK);
    assert(!g_s.lock && target_cycles==0);
    touch_lock_buttons(0,false); assert(!touch_lock_on());
    g_s.layout.x[C_LOCK]=.31f; g_s.layout.y[C_LOCK]=.52f; g_s.layout.scale[C_LOCK]=1.2f;
    assert(save_layout_file(g_s.layout,"lock-control.txt"));
    Layout stored; assert(load_layout_file(&stored,"lock-control.txt")); same_layout(stored,g_s.layout);
    assert(TOUCH_ICON_BUTTON_COUNT==25 && ICON_CONTROLS[24]==C_LOCK);
    assert(!strcmp(touch_icon_button_name(24),"Lock large monster"));
    puts("PASS: standalone lock button toggles nearest-large-monster lock without cycling or emitting R3 and persists in layouts");
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
    puts("PASS: every Show* binding across common menu/quest/town contexts controls native drawing and hits, including town ZL, guard and item arrows");
}
static void common_startup_and_legacy() {
    fresh(); remove(COMMON_PATH);
    std::vector<std::string> originals;
    for(int p=0;p<LAYOUT_SLOTS;++p) {
        Layout old=defaults(p); old.x[C_DPAD_TOGGLE]=.21f+p*.01f;
        old.x[C_RSTICK]=.333f; old.y[C_RSTICK]=.444f; old.scale[C_RSTICK]=1.23f;
        old.zx[TZ_ITEM]=p*2; old.zy[TZ_TARGET]=-p;
        assert(save_legacy_layout(old,p)); originals.push_back(stored_layout_text(p));
    }
    write_text("touch_weapon_presets.txt","5 20\n9 21\nselected 18\n");
    const std::string old_selection=read_text("touch_weapon_presets.txt");
    Layout imported; assert(load_layout(&imported,COMMON_BASE)); imported.style=STYLE_SEPARATE; imported.weapon_profile=-1;
    for(int old_profile:{0,5,15,16,18,19,23,999}) for(bool quest:{false,true}) {
        touch_reset(); g_s={}; profile=old_profile; in_quest=quest; update();
        assert(profile==TOUCH_UNIVERSAL_PROFILE && g_s.layout_profile==UNIVERSAL_LAYOUT && g_style==STYLE_SEPARATE && !g_ranged);
        same_layout(g_s.layout,imported);
        for(int weapon:{0,6,10,15,UNKNOWN_WEAPON}) for(bool menu:{false,true}) {
            weapon_override=weapon; ranged=weapon==6?WEAPON_BOWGUN:weapon==10?WEAPON_BOW:0; drawn=1;
            game_menu_state(menu); same_layout(g_s.layout,imported);
            assert(profile==TOUCH_UNIVERSAL_PROFILE && g_s.detected_weapon==(quest?weapon_type():-1));
        }
        game_menu_state(false);
    }
    assert(!SDL_GetPathInfo(COMMON_PATH,nullptr));
    Layout saved=imported; saved.x[C_DPAD_TOGGLE]=.6789f; saved.zx[TZ_ITEM]=-31; saved.zy[TZ_ARTS]=48;
    assert(save_layout_file(saved,COMMON_PATH));
    touch_reset(); g_s={}; update(); same_layout(g_s.layout,saved);
    Layout later=defaults(COMMON_BASE); later.x[C_DPAD_TOGGLE]=.913f;
    assert(save_legacy_layout(later,COMMON_BASE));
    touch_reset(); g_s={}; update(); same_layout(g_s.layout,saved);
    assert(save_legacy_layout(imported,COMMON_BASE)); originals[COMMON_BASE]=stored_layout_text(COMMON_BASE);
    const std::string good=read_text(COMMON_PATH);
    write_text(COMMON_PATH,"layout_version 1\nattack 0.9 0.5 1\n");
    touch_reset(); g_s={}; update(); same_layout(g_s.layout,defaults(COMMON_BASE));
    assert(read_text(COMMON_PATH)=="layout_version 1\nattack 0.9 0.5 1\n");
    write_text(COMMON_PATH,good);
    for(int p=0;p<LAYOUT_SLOTS;++p) assert(stored_layout_text(p)==originals[p]);
    assert(read_text("touch_weapon_presets.txt")==old_selection);
    puts("PASS: common startup imports only missing legacy16, always uses profile19/separate geometry across quests/menus/weapons/restart, prioritizes existing common data and leaves every legacy file/selection unchanged; corrupt common falls back without overwriting it");
}
static void common_r_aim_identity() {
    fresh();
    assert(!strcmp(DEFS[C_R].key,"guard") && !strcmp(DEFS[C_R].name,"Aim  R"));
    assert(DEFS[C_R].icon==I_SCOPE && DEFS[C_R].buttons==B(BTN_R) && !strcmp(DEFS[C_R].letter,"R"));
    assert(!strcmp(settings_text(2,DEFS[C_R].name),"瞄准  R"));
    assert(DEFS[C_R].x==.505f && DEFS[C_R].y==.095f && DEFS[C_R].r==.058f);
    Layout aligned=g_s.layout;
    aligned.x[C_RAIM]=aligned.x[C_R]; aligned.y[C_RAIM]=aligned.y[C_R];
    aligned.scale[C_RAIM]=DEFS[C_R].r/DEFS[C_RAIM].r;
    auto drawing=[&](int c) {
        ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=ImVec2((float)screen_w,(float)screen_h); io.DeltaTime=1.0f/60;
        ImGui::NewFrame(); ImDrawList *list=ImGui::GetBackgroundDrawList();
        control(list,ImGui::GetFont(),aligned,c,(float)screen_w,(float)screen_h,g_s,1);
        std::vector<ImDrawVert> vertices(list->VtxBuffer.begin(),list->VtxBuffer.end());
        ImGui::Render(); return vertices;
    };
    const auto common=drawing(C_R), scope=drawing(C_RAIM);
    assert(common.size()==scope.size() && common.size()>100);
    for(size_t i=0;i<common.size();++i) {
        assert(fabsf(common[i].pos.x-scope[i].pos.x)<.02f && fabsf(common[i].pos.y-scope[i].pos.y)<.02f);
        assert(common[i].col==scope[i].col);
    }
    for(bool quest:{false,true}) for(int weapon:{0,6,10}) {
        fresh(); in_quest=quest; weapon_override=weapon; ranged=weapon==6?WEAPON_BOWGUN:weapon==10?WEAPON_BOW:0;
        local_hunter=0x1234; drawn=1; update();
        at(SDL_EVENT_FINGER_DOWN,C_R); const Input held=update(B(BTN_L),true);
        assert(held.touch_b==B(BTN_R) && held.b==(B(BTN_L)|B(BTN_R)));
        at(SDL_EVENT_FINGER_CANCELED,C_R); assert(update(B(BTN_L),true).b==B(BTN_L));
    }
    puts("PASS: common R is localized Aim with the original golden scope geometry/colors; guard persistence key, position/size and touch/controller BTN_R behavior remain intact across town/quest weapon contexts");
}
static void ammo_default_migration() {
    fresh(); enable_universal_combos(); assert(save_layout_file(g_s.layout,COMMON_PATH));
    const std::string original=read_text(COMMON_PATH),old_legacy=stored_layout_text(COMMON_BASE);
    char legacy_path[40]; layout_path(COMMON_BASE,legacy_path,sizeof legacy_path);
    for(int version:{0,1,2,3,4}) for(int up=0;up<3;++up) for(int down=0;down<3;++down) {
        Layout source=defaults(COMMON_BASE); source.x[C_DPAD_TOGGLE]=.717f;
        for(int c:{C_AMMO_UP,C_AMMO_DOWN}) {
            const int custom=c==C_AMMO_UP?up:down;
            source.x[c]=.56f+(custom==1?.017f:0); source.y[c]=c==C_AMMO_UP?.30f:.13f;
            source.scale[c]=custom==2?1.25f:1;
        }
        Layout expected=source;
        for(int c:{C_AMMO_UP,C_AMMO_DOWN}) if(version<3 && (c==C_AMMO_UP?up:down)==0) {
            expected.x[c]=.94f; expected.y[c]=c==C_AMMO_UP?.44f:.28f;
        }
        std::string stored;
        if(!version) {
            assert(save_legacy_layout(source,COMMON_BASE)); stored=stored_layout_text(COMMON_BASE); assert(SDL_RemovePath(COMMON_PATH));
        } else {
            g_s.layout=source; assert(touch_template_save(9,"Ammo migration"));
            stored=read_text("touch_template_10.txt"); if(version<4) stored=legacy_template(stored,version);
            write_text(COMMON_PATH,stored); write_text("touch_template_10.txt",stored);
        }
        Layout loaded; load_common_layout(&loaded); same_layout(loaded,expected);
        touch_reset(); g_s={}; update(); same_layout(g_s.layout,expected);
        if(!version) {
            assert(!SDL_GetPathInfo(COMMON_PATH,nullptr) && stored_layout_text(COMMON_BASE)==stored);
            assert(save_layout_file(expected,COMMON_PATH));
        } else {
            assert(read_text(COMMON_PATH)==stored);
            assert(touch_template_use(9)); same_layout(g_s.layout,expected);
            assert(read_text("touch_template_10.txt")==stored);
        }
    }
    write_text(COMMON_PATH,original); write_text(legacy_path,old_legacy); touch_reset(); g_s={}; update();
    puts("PASS: v1/v2 and missing-common legacy import migrate each exact old ammo default independently; any custom position or scale survives, v3 old-looking positions stay unchanged, and load/startup/template application never rewrites the source snapshot");
}
static void common_controls_and_names() {
    const char *const names[]={"蓄力", "回避 组合", "锁定目标", "防御", "道具", "狩技三"};
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) {
        fresh((int)size.x,(int)size.y); cfg.touch_auto_r=0; enable_universal_combos();
        for(int i=0;i<TOUCH_COMBOS;++i) strcpy(cfg.touch_combo_name[i],names[i]);
        std::fill(std::begin(cfg.touch_show),std::end(cfg.touch_show),1);
        for(bool quest:{false,true}) for(int weapon:{0,6,10,15}) for(bool menu:{false,true}) {
            in_quest=quest; weapon_override=weapon; ranged=weapon==6?WEAPON_BOWGUN:weapon==10?WEAPON_BOW:0;
            game_menu_state(menu);
            assert(profile==TOUCH_UNIVERSAL_PROFILE && g_style==STYLE_SEPARATE && !g_ranged);
            for(int c:{C_LAYOUT,C_RFIRE,C_RSECOND,C_RAIM,C_RSPECIAL,C_RDODGE,C_RSHEATHE}) {
                assert(!in_mode(c,menu,quest) && !in_editor(c,quest,profile));
            }
            assert(!in_mode(C_RSTICK,menu,quest) && in_editor(C_RSTICK,quest,profile));
            for(int c:{C_AMMO_UP,C_AMMO_DOWN}) {
                assert(in_mode(c,menu,quest)==(quest && ranged!=0 && !menu));
                assert(in_editor(c,quest,profile)==(quest && ranged!=0));
            }
            const auto geometry=touch_geometry(); const float old=g_s.layout.x[C_LAYOUT];
            g_s.layout.x[C_LAYOUT]=.57f; same_geometry(geometry,touch_geometry()); g_s.layout.x[C_LAYOUT]=old;
            const Geo retired=place(g_s.layout,C_LAYOUT,(float)screen_w,(float)screen_h);
            event(SDL_EVENT_FINGER_DOWN,8,retired.c.x,retired.c.y);
            const Finger *f=finger(g_s,8); assert(!f || f->ctl!=C_LAYOUT);
            event(SDL_EVENT_FINGER_CANCELED,8,retired.c.x,retired.c.y); update();
            for(int i=0;i<TOUCH_COMBOS;++i) {
                const int c=C_COMBO+i; char label[64]; combo_label(c,profile,label,sizeof label); assert(!strcmp(label,names[i]));
                assert(in_mode(c,menu,quest)); const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
                assert(drawn_vertices(g.c,g.r*.6f)>0);
                at(SDL_EVENT_FINGER_DOWN,c); const Input held=update(B(BTN_B),true);
                assert(held.touch_b==config_combo_mask(UNIVERSAL_COMBOS[i]) && held.b==(held.touch_b|B(BTN_B)));
                at(SDL_EVENT_FINGER_CANCELED,c); assert(update(B(BTN_B),true).b==B(BTN_B));
            }
        }
        for(int i=0;i<TOUCH_COMBOS;++i) {
            cfg.touch_combo_name[i][0]=0; char label[64]; combo_label(C_COMBO+i,profile,label,sizeof label);
            assert(!strcmp(label,UNIVERSAL_COMBOS[i])); strcpy(cfg.touch_combo_name[i],names[i]);
            const uint32_t before=config_live_touch_combo(profile,i);
            strcpy(cfg.touch_combo_name[i],"同名"); assert(config_live_touch_combo(profile,i)==before);
            cfg.touch_weapon_combo[profile][i][0]=0; assert(!in_mode(C_COMBO+i,g_s.menu,g_s.in_quest));
            strcpy(cfg.touch_weapon_combo[profile][i],UNIVERSAL_COMBOS[i]);
        }
        char small[5]={}; strcpy(cfg.touch_combo_name[0],"蓄力辅助"); combo_label(C_COMBO,profile,small,sizeof small);
        assert(!strcmp(small,"蓄"));
    }
    puts("PASS: one common layout and six independent named combos stay active across all scene/weapon/menu contexts and four sizes; removed layout/ranged/right-stick controls neither draw nor hit; UTF-8 labels, blank fallback and renamed/empty binding semantics are independent");
}
static void common_editor_and_failure() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) for(bool quest:{false,true}) {
        fresh((int)size.x,(int)size.y); in_quest=quest; update(); enable_universal_combos();
        assert(save_layout_file(g_s.layout,COMMON_PATH)); const Layout original=g_s.layout;
        touch_edit_begin(); update(); assert(g_s.edit && E_COUNT==5);
        same_layout(g_s.layout,original);
        Rect panel,buttons[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,buttons);
        assert(panel.x0>=0 && panel.y0>=0 && panel.x1<=screen_w && panel.y1<=screen_h);
        for(int c=0;c<C_LAID;++c) {
            if(!in_editor(c,quest,profile)) continue;
            editor_control_art_present(c);
        }
        assert(!in_editor(C_LAYOUT,quest,profile));
        editor_click(E_CANCEL); update();
        const int editable[]={C_STICK,C_HEAVY,C_LIGHT,C_R,C_DPAD_TOGGLE,C_HDPAD,C_COMBO,C_COMBO+5};
        for(int c:editable) {
            if(!in_editor(c,quest,profile)) continue;
            touch_edit_begin(); update(); const Geo g=place(g_s.layout,c,(float)screen_w,(float)screen_h);
            ImVec2 point=g.c; if(c==C_HDPAD) point.x+=g.r*.6f;
            editor_panel_away(point);
            event(SDL_EVENT_FINGER_DOWN,1,point.x,point.y); assert(g_s.sel==c && finger(g_s,1)->kind==F_EDIT);
            event(SDL_EVENT_FINGER_MOTION,1,point.x-8,point.y+7); event(SDL_EVENT_FINGER_UP,1,point.x-8,point.y+7);
            assert(g_s.layout.x[c]!=original.x[c] || g_s.layout.y[c]!=original.y[c]);
            editor_click(E_CANCEL); update(); same_layout(g_s.layout,original);
        }
    }
    fresh(); assert(save_layout_file(g_s.layout,COMMON_PATH)); const std::string old=read_text(COMMON_PATH);
    touch_edit_begin(); update(); g_s.layout.x[C_DPAD_TOGGLE]=.6123f; const Layout draft=g_s.layout;
    const char *temp="touch_layout_common.txt.tmp"; assert(!SDL_GetPathInfo(temp,nullptr)); assert(SDL_CreateDirectory(temp));
    editor_click(E_SAVE); update(); assert(g_s.edit && g_s.save_failed); same_layout(g_s.layout,draft); assert(read_text(COMMON_PATH)==old);
    in_quest=false; update(); assert(g_s.edit && g_s.save_failed); same_layout(g_s.layout,draft); assert(read_text(COMMON_PATH)==old);
    in_quest=true; update(); assert(g_s.edit); same_layout(g_s.layout,draft); assert(read_text(COMMON_PATH)==old);
    assert(SDL_RemovePath(temp)); editor_click(E_SAVE); update(); assert(!g_s.edit && !g_s.save_failed);
    Layout saved; assert(load_layout_file(&saved,COMMON_PATH)); same_layout(saved,draft);
    touch_edit_begin(); update(); editor_click(E_RESET); same_layout(g_s.layout,defaults(COMMON_BASE));
    editor_click(E_CANCEL); update(); same_layout(g_s.layout,draft);
    touch_edit_begin(); update(); g_s.layout.y[C_DPAD_TOGGLE]=.4123f; const Layout across=g_s.layout;
    const std::string saved_disk=read_text(COMMON_PATH);
    in_quest=false; update(); assert(g_s.edit); same_layout(g_s.layout,across); assert(read_text(COMMON_PATH)==saved_disk);
    editor_click(E_CANCEL); update(); assert(!g_s.edit); same_layout(g_s.layout,draft);
    touch_edit_begin(); update(); g_s.layout=across;
    in_quest=true; update(); assert(g_s.edit); same_layout(g_s.layout,across); assert(read_text(COMMON_PATH)==saved_disk);
    editor_click(E_SAVE); update(); assert(!g_s.edit);
    touch_reset(); g_s={}; update(); same_layout(g_s.layout,across);
    puts("PASS: common editor has five controls and selectable native buttons at four sizes; Save/Cancel/Defaults work; scene changes preserve unsaved and failed-save drafts plus prior disk data, and remain explicitly saveable/cancelable");
}
static void template_roundtrip_and_failure() {
    fresh(); enable_universal_combos(); cfg.volume=73;
    assert(TOUCH_TEMPLATE_COUNT==40);
    for(int p=0;p<TOUCH_UNIVERSAL_PROFILE;++p) strcpy(cfg.touch_weapon_combo[p][0],"Y");
    assert(save_layout_file(g_s.layout,COMMON_PATH)); const std::string active=read_text(COMMON_PATH);
    std::vector<Layout> layouts; std::vector<Config> configurations; std::vector<std::string> stored;
    for(int slot=0;slot<TOUCH_TEMPLATE_COUNT;++slot) {
        char path[40],expected[40]; template_path(slot,path,sizeof path);
        snprintf(expected,sizeof expected,"touch_template_%02d.txt",slot+1);
        assert(!strcmp(path,expected)); remove(path);
    }
    for(int slot=0;slot<TOUCH_TEMPLATE_COUNT;++slot) {
        if(slot==20) {
            for(int old=0;old<20;++old) { char path[40]; template_path(old,path,sizeof path); assert(read_text(path)==stored[old]); }
            for(int added=20;added<40;++added) {
                char name[64]="stale"; assert(!touch_template_info(added,name,sizeof name) && !name[0]);
            }
        }
        char path[40],name[64],actual[64]; template_path(slot,path,sizeof path);
        assert(!touch_template_info(slot,actual,sizeof actual) && !actual[0]);
        g_s.layout=defaults(COMMON_BASE); g_s.layout.x[C_DPAD_TOGGLE]=.31f+slot*.01f;
        g_s.layout.scale[C_STICK]=.7f+slot*.02f; g_s.layout.zx[TZ_ITEM]=slot*3; g_s.layout.zy[TZ_ARTS]=-slot*2;
        g_s.layout.zx[TZ_MAP]=slot*5; g_s.layout.zy[TZ_MAP]=-slot*4;
        g_s.layout.editor_x=slot/39.f; g_s.layout.editor_y=(39-slot)/39.f;
        for(int c:{C_AMMO_UP,C_AMMO_DOWN}) {
            g_s.layout.x[c]=.90f+slot*.002f+(c==C_AMMO_DOWN?.10f:0);
            g_s.layout.y[c]=.20f+slot*.003f+(c==C_AMMO_DOWN?.20f:0);
            g_s.layout.scale[c]=.60f+slot*.01f+(c==C_AMMO_DOWN?.20f:0);
            g_s.layout.icons[c].choice=(slot+(c==C_AMMO_DOWN?3:0))%touch_icon_builtin_count()+1;
        }
        for(int i=0;i<TOUCH_COMBOS;++i) {
            strcpy(cfg.touch_weapon_combo[profile][i],UNIVERSAL_COMBOS[(slot+i)%TOUCH_COMBOS]);
            cfg.touch_weapon_combo_pad[profile][i]=(slot+i)%PADB_COUNT;
            snprintf(cfg.touch_combo_name[i],TOUCH_COMBO_NAME_MAX,"模板%d 组合%d",slot+1,i+1);
        }
        if(slot==2) { cfg.touch_weapon_combo[profile][0][0]=0; cfg.touch_combo_name[0][0]=0; }
        if(slot==39) {
            const char *const names[]={"  蓄力", "回避  ", "  组合 中文  ", "", "   ", "普通 UTF-8 名称"};
            for(int i=0;i<TOUCH_COMBOS;++i) strcpy(cfg.touch_combo_name[i],names[i]);
        }
        snprintf(name,sizeof name,"自定义 %d",slot+1);
        layouts.push_back(g_s.layout); configurations.push_back(cfg);
        assert(touch_template_save(slot,name) && touch_template_info(slot,actual,sizeof actual) && !strcmp(name,actual));
        stored.push_back(read_text(path)); assert(read_text(COMMON_PATH)==active);
    }
    for(int slot=0;slot<TOUCH_TEMPLATE_COUNT;++slot) {
        cfg.volume=37;
        assert(touch_template_use(slot)); same_layout(g_s.layout,layouts[slot]); same_combos(cfg,configurations[slot]);
        assert(cfg.volume==37 && profile==TOUCH_UNIVERSAL_PROFILE);
        for(int p=0;p<TOUCH_UNIVERSAL_PROFILE;++p) assert(!strcmp(cfg.touch_weapon_combo[p][0],"Y"));
        Layout disk; assert(load_layout_file(&disk,COMMON_PATH)); same_layout(disk,layouts[slot]);
        touch_reset(); g_s={}; update(); same_layout(g_s.layout,layouts[slot]);
    }
    for(int slot=0;slot<TOUCH_TEMPLATE_COUNT;++slot) { char path[40]; template_path(slot,path,sizeof path); assert(read_text(path)==stored[slot]); }
    // The last slot is an explicit snapshot; active edits cannot silently overwrite it or old slots.
    g_s.layout.x[C_DPAD_TOGGLE]+=.01f;
    assert(read_text("touch_template_40.txt")==stored[39]);
    assert(touch_template_use(39)); same_layout(g_s.layout,layouts[39]);
    assert(read_text("touch_template_01.txt")==stored[0] && read_text("touch_template_20.txt")==stored[19]);
    const int invalid_slots[]={-1,TOUCH_TEMPLATE_COUNT,999};
    char out[64]; for(int slot:invalid_slots) assert(!touch_template_info(slot,out,sizeof out) && !touch_template_save(slot,"bad") && !touch_template_use(slot));
    for(const char *name:{"","   ","bad\nname","bad\tname"}) assert(!touch_template_save(0,name));
    const std::string longname(64,'a'); assert(!touch_template_save(0,longname.c_str()));
    char shortname[5]; assert(touch_template_info(0,shortname,sizeof shortname) && !strcmp(shortname,"自"));
    const Config before_cfg=cfg; const Layout before=g_s.layout; const std::string before_disk=read_text(COMMON_PATH);
    touch_edit_begin(); update(); g_s.layout.x[C_COMBO]=.733f; const Layout draft=g_s.layout;
    strcpy(cfg.touch_combo_name[0],"编辑草稿"); const Config draft_cfg=cfg;
    assert(touch_template_save(0,"编辑保存") && g_s.edit); assert(!touch_template_use(1));
    same_layout(g_s.layout,draft); assert(read_text(COMMON_PATH)==before_disk);
    editor_click(E_CANCEL); update(); same_layout(g_s.layout,before);
    assert(touch_template_use(0)); same_layout(g_s.layout,draft); same_combos(cfg,draft_cfg);
    const std::string saved_template=read_text("touch_template_01.txt");
    assert(SDL_CreateDirectory("touch_template_01.txt.tmp"));
    g_s.layout.x[C_COMBO]=.932f; assert(!touch_template_save(0,"覆盖失败"));
    assert(read_text("touch_template_01.txt")==saved_template); assert(SDL_RemovePath("touch_template_01.txt.tmp"));
    g_s.layout=before; cfg=before_cfg; assert(save_layout_file(before,COMMON_PATH));
    const std::string failed_disk=read_text(COMMON_PATH);
    for(int mode=0;mode<3;++mode) {
        if(mode==0) config_save_fail=true;
        if(mode==1) companion_commit_fail=true;
        if(mode==2) assert(SDL_CreateDirectory("touch_layout_common.txt.apply.tmp"));
        assert(!touch_template_use(0)); same_layout(g_s.layout,before); same_combos(cfg,before_cfg);
        assert(read_text(COMMON_PATH)==failed_disk && read_text("touch_template_01.txt")==saved_template);
        config_save_fail=companion_commit_fail=false;
        if(mode==2) assert(SDL_RemovePath("touch_layout_common.txt.apply.tmp"));
    }
    at(SDL_EVENT_FINGER_DOWN,C_HEAVY,1); at(SDL_EVENT_FINGER_MOTION,C_HEAVY,1,2,-1);
    at(SDL_EVENT_FINGER_DOWN,C_STICK,2); at(SDL_EVENT_FINGER_MOTION,C_STICK,2,2,0); pulse(g_s,B(BTN_Y),1000*MS);
    assert(touch_template_use(0)); assert(!finger(g_s,1) && !finger(g_s,2)); neutral_sticks(update()); assert(!update().b);
    puts("PASS: all 40 custom templates independently round-trip geometry/HUD plus six combo masks/pads/UTF-8 names, including exact leading/trailing spaces, empty and all-space names; original slots 1-20 keep their paths, slots 21-40 start empty, slot40 stays independent of slots1/20 and active edits, and slot41 is rejected; explicit draft snapshots and failed transactions preserve unrelated config/templates");
}
static void map_fixed_storage() {
    fresh(); enable_universal_combos(); g_s.layout.zx[TZ_MAP]=123; g_s.layout.zy[TZ_MAP]=-87;
    g_s.layout.editor_x=.1234f; g_s.layout.editor_y=.8765f;
    assert(save_layout_file(g_s.layout,COMMON_PATH) && touch_template_save(0,"Map and toolbar"));
    const Layout before=g_s.layout; const Config before_cfg=cfg;
    const std::string active=read_text(COMMON_PATH),snapshot=read_text("touch_template_01.txt");
    auto replace=[](std::string text,const std::string &key,const std::string &value) {
        const size_t begin=text.find(key); assert(begin!=std::string::npos);
        const size_t end=text.find('\n',begin); assert(end!=std::string::npos); return text.replace(begin,end-begin+1,value);
    };
    assert(active.find("layout_version 5")!=std::string::npos && active.find("hud_map 123.0 -87.0 1")!=std::string::npos);
    assert(active.find("editor_panel 0.1234 0.8765 1")!=std::string::npos);
    for(int version:{1,2,3,4}) {
        const std::string common=legacy_template(active,version);
        const std::string saved=legacy_template(snapshot,version);
        Layout expected=before;
        if(version<4) { expected.zx[TZ_MAP]=expected.zy[TZ_MAP]=0; expected.editor_x=.5f; expected.editor_y=0; }
        write_text(COMMON_PATH,common); Layout loaded; assert(load_layout_file(&loaded,COMMON_PATH)); same_layout(loaded,expected);
        load_common_layout(&loaded); same_layout(loaded,expected); assert(read_text(COMMON_PATH)==common);
        write_text("touch_template_01.txt",saved); char name[64];
        assert(touch_template_info(0,name,sizeof name) && touch_template_use(0)); same_layout(g_s.layout,expected);
        assert(editor_hud_rect(TZ_MAP).ok);
        assert(read_text("touch_template_01.txt")==saved); // Reading old snapshots never upgrades or overwrites them.
        g_s.layout=before; cfg=before_cfg; write_text(COMMON_PATH,active);
        std::vector<std::string> corrupt;
        for(const char *field:{"hud_items ","hud_arts ","hud_target "}) corrupt.push_back(replace(saved,field,""));
        if(version==4) {
            corrupt.push_back(replace(saved,"hud_map ","")); corrupt.push_back(replace(saved,"editor_panel ",""));
        }
        std::string without=version<4?saved:replace(replace(saved,"hud_map ",""),"editor_panel ","");
        const char *const bad[]={"hud_map nan 0 1\n","hud_map 0 inf 1\n","hud_map 1401 0 1\n","hud_map 0 -801 1\n",
            "hud_map 1 2\n","hud_map 1 2 2\n","hud_map 1 2 1 extra\n","hud_map 0 0 1\nhud_map 1 2 1\n",
            "editor_panel nan 0 1\n","editor_panel 0 inf 1\n","editor_panel -.1 .5 1\n","editor_panel .5 1.1 1\n",
            "editor_panel .5\n","editor_panel .5 0 0\n","editor_panel .5 0 1 extra\n","editor_panel 0 0 1\neditor_panel 1 1 1\n"};
        for(const char *line:bad) corrupt.push_back(without+line);
        for(const std::string &text:corrupt) {
            write_text("touch_template_01.txt",text); name[0]='x';
            assert(!touch_template_info(0,name,sizeof name) && !name[0] && !touch_template_use(0));
            same_layout(g_s.layout,before); same_combos(cfg,before_cfg); assert(read_text(COMMON_PATH)==active && read_text("touch_template_01.txt")==text);
            write_text("touch-map-invalid.txt",text); loaded=before;
            assert(!load_layout_file(&loaded,"touch-map-invalid.txt")); same_layout(loaded,before);
        }
        if(version<4) {
            write_text("touch_template_01.txt",saved+"hud_map 25 -35 1\neditor_panel 0 1 1\n");
            assert(touch_template_use(0)); expected.zx[TZ_MAP]=25; expected.zy[TZ_MAP]=-35; expected.editor_x=0; expected.editor_y=1;
            same_layout(g_s.layout,expected); assert(editor_hud_rect(TZ_MAP).ok);
            g_s.layout=before; cfg=before_cfg; write_text(COMMON_PATH,active);
        }
    }
    for(const char *field:{"right_stick_toggle ","lock ","icon lock "}) {
        const std::string missing=replace(snapshot,field,"");
        write_text("touch-map-invalid.txt",missing); Layout loaded=before;
        assert(!load_layout_file(&loaded,"touch-map-invalid.txt")); same_layout(loaded,before);
    }
    write_text("touch_template_01.txt",snapshot); remove("touch-map-invalid.txt");
    puts("PASS: map/storage migration: v4 keeps map/editor offsets; v1-v4 files default the later right-stick toggle, lock control and lock icon without rewriting snapshots; v5 requires its new records and preserves invalid-field atomicity");
}
static void editor_toolbar_dragging() {
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(720,1280)}) {
        fresh((int)size.x,(int)size.y); assert(save_layout_file(g_s.layout,COMMON_PATH));
        touch_edit_begin(); update(); Rect panel,buttons[E_COUNT];
        editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,buttons);
        assert(E_COUNT==5 && g_s.layout.editor_x==.5f && !g_s.layout.editor_y);
        assert(fabsf(panel.x1-panel.x0-std::min(.94f*screen_w,1.5f*screen_h))<.02f);
        for(int i=0;i<E_COUNT;++i) {
            assert(fabsf(buttons[i].y1-buttons[i].y0-std::max(.10f*screen_h,48.f))<.02f);
            assert(buttons[i].x0>panel.x0 && buttons[i].x1<panel.x1 && buttons[i].y1<panel.y1);
            if(i) assert(buttons[i-1].x1<buttons[i].x0);
        }
        assert(buttons[0].y0-panel.y0>=std::max(.06f*screen_h,28.f));
        const ImVec2 old_reset((buttons[E_RESET].x0+buttons[E_RESET].x1)*.5f,(buttons[E_RESET].y0+buttons[E_RESET].y1)*.5f);
        g_s.sel=C_STICK; const Layout selected=g_s.layout;
        editor_move_panel(-2,3); assert(g_s.sel==C_STICK); // The entire panel clamps even after overshooting the screen.
        Layout unchanged=g_s.layout; unchanged.editor_x=selected.editor_x; unchanged.editor_y=selected.editor_y; same_layout(unchanged,selected);
        editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,buttons);
        assert(panel.x0>=0 && panel.y0>=0 && panel.x1<=screen_w+.01f && panel.y1<=screen_h+.01f);
        const Layout moved=g_s.layout;
        event(SDL_EVENT_FINGER_DOWN,1,old_reset.x,old_reset.y); event(SDL_EVENT_FINGER_UP,1,old_reset.x,old_reset.y);
        same_layout(g_s.layout,moved); // The previous toolbar location is no longer an action area.
        g_s.sel=C_STICK; editor_click(E_BIGGER); assert(g_s.layout.scale[C_STICK]>moved.scale[C_STICK]+.09f);
        const auto geometry=touch_geometry(); assert(geometry.size()>100);
        const ImVec2 heading=editor_header(); assert(drawn_vertices(heading,std::max(28.f,.06f*screen_h))>0);
        const Layout draft=g_s.layout; editor_click(E_SAVE); update();
        Layout stored; assert(load_layout_file(&stored,COMMON_PATH)); same_layout(stored,draft);
        touch_edit_begin(); update(); editor_move_panel(1,0); editor_click(E_CANCEL); update(); same_layout(g_s.layout,draft);
        touch_edit_begin(); update(); editor_click(E_RESET); same_layout(g_s.layout,defaults(COMMON_BASE));
        editor_click(E_CANCEL); update(); same_layout(g_s.layout,draft);
        touch_reset(); g_s={}; update(); same_layout(g_s.layout,draft);
    }
    for(int object:{(int)C_HEAVY,ZONE_SEL+TZ_ITEM}) for(bool panel_first:{false,true}) {
        fresh(); editor_isolate_hud();
        touch_hud_piece(TZ_ITEM,500,380,500,380,0,0); touch_hud_frame();
        g_s.layout.x[C_HEAVY]=.45f; g_s.layout.y[C_HEAVY]=.35f;
        assert(save_layout_file(g_s.layout,COMMON_PATH)); touch_edit_begin(); update();
        const ImVec2 p=object==C_HEAVY?place(g_s.layout,C_HEAVY,(float)screen_w,(float)screen_h).c:editor_hud_pick(TZ_ITEM);
        editor_panel_away(p); const ImVec2 h=editor_header(); const Layout before=g_s.layout;
        if(panel_first) event(SDL_EVENT_FINGER_DOWN,2,h.x,h.y);
        event(SDL_EVENT_FINGER_DOWN,1,p.x,p.y);
        if(!panel_first) event(SDL_EVENT_FINGER_DOWN,2,h.x,h.y);
        assert(finger(g_s,2)->kind==F_EDIT_PANEL && !g_s.pinch);
        assert(finger(g_s,1)->kind==(panel_first?F_MENU:F_EDIT));
        const int selection=g_s.sel;
        event(SDL_EVENT_FINGER_MOTION,2,h.x+13,h.y+17);
        Layout panel_only=g_s.layout; panel_only.editor_x=before.editor_x; panel_only.editor_y=before.editor_y; same_layout(panel_only,before);
        assert(g_s.sel==selection && !g_s.pinch);
        const float panel_x=g_s.layout.editor_x,panel_y=g_s.layout.editor_y;
        event(SDL_EVENT_FINGER_MOTION,1,p.x-11,p.y+9);
        assert(g_s.layout.editor_x==panel_x && g_s.layout.editor_y==panel_y && !g_s.pinch);
        for(int c=0;c<C_COUNT;++c) assert(g_s.layout.scale[c]==before.scale[c]);
        if(panel_first) { panel_only=g_s.layout; panel_only.editor_x=before.editor_x; panel_only.editor_y=before.editor_y; same_layout(panel_only,before); }
        else if(object==C_HEAVY) assert(g_s.layout.x[C_HEAVY]!=before.x[C_HEAVY]);
        else assert(g_s.layout.zx[TZ_ITEM]!=before.zx[TZ_ITEM]);
        event(SDL_EVENT_FINGER_CANCELED,2,h.x+13,h.y+17); event(SDL_EVENT_FINGER_CANCELED,1,p.x-11,p.y+9);
        const Layout cancelled=g_s.layout;
        event(SDL_EVENT_FINGER_UP,2,h.x,h.y); event(SDL_EVENT_FINGER_UP,1,p.x,p.y); same_layout(g_s.layout,cancelled);
        assert(!update().b && !update().touch_b); editor_click(E_CANCEL); update();
    }
    fresh(); assert(save_layout_file(g_s.layout,COMMON_PATH)); const std::string disk=read_text(COMMON_PATH);
    touch_edit_begin(); update(); editor_move_panel(.23f,.72f); const Layout draft=g_s.layout;
    assert(SDL_CreateDirectory("touch_layout_common.txt.tmp")); editor_click(E_SAVE); update();
    assert(g_s.edit && g_s.save_failed && read_text(COMMON_PATH)==disk); same_layout(g_s.layout,draft);
    assert(SDL_RemovePath("touch_layout_common.txt.tmp")); editor_click(E_SAVE); update(); assert(!g_s.edit && !g_s.save_failed);
    touch_edit_begin(); update(); const ImVec2 h=editor_header();
    touch_hud_piece(TZ_ITEM,h.x,h.y,h.x,h.y,0,0); touch_hud_frame(); assert(editor_hud_rect(TZ_ITEM).in(h.x,h.y));
    event(SDL_EVENT_FINGER_DOWN,1,h.x,h.y); assert(finger(g_s,1)->kind==F_EDIT_PANEL);
    event(SDL_EVENT_FINGER_CANCELED,1,h.x,h.y); // Android cancels active contacts when its surface/orientation changes.
    screen_w=720; screen_h=1280; assert(SDL_SetWindowSize(window,screen_w,screen_h)); SDL_PumpEvents(); g_render_aspect=(float)screen_w/screen_h; update();
    assert(!finger(g_s,1)); same_layout(g_s.layout,draft);
    Rect panel,buttons[E_COUNT]; editor_panel(g_s.layout,(float)screen_w,(float)screen_h,&panel,buttons);
    assert(panel.x0>=0 && panel.y0>=0 && panel.x1<=screen_w && panel.y1<=screen_h);
    event(SDL_EVENT_FINGER_UP,1,h.x,h.y); same_layout(g_s.layout,draft); editor_click(E_CANCEL); update();
    puts("PASS: editor-toolbar-dragging: five enlarged buttons and a separate header render at four sizes; drag/clamp and moved hit areas preserve selected controls, two-finger panel/control/item-HUD ownership never pinches, Save/Cancel/Defaults/retry persist normalized coordinates, and portrait resize preserves position without reviving cancelled fingers");
}
static void template_corrupt_input() {
    fresh(); enable_universal_combos(); assert(save_layout_file(g_s.layout,COMMON_PATH)); assert(touch_template_save(0,"有效模板"));
    const std::string good=read_text("touch_template_01.txt"),active=read_text(COMMON_PATH); const Layout before=g_s.layout; const Config before_cfg=cfg;
    auto line=[](std::string text,const std::string &key,const std::string &value) {
        const size_t begin=text.find(key); assert(begin!=std::string::npos); const size_t end=text.find('\n',begin); assert(end!=std::string::npos);
        return text.replace(begin,end-begin+1,value);
    };
    const std::vector<std::string> bad={"",good.substr(0,good.size()/2),line(good,"layout_version ","layout_version 999\n"),
        line(good,"template_name ","template_name \n"),line(good,"attack ",""),line(good,"hud_items ","hud_items nan 0 1\n"),
        line(good,"combo_mask 1 ",""),line(good,"combo_mask 1 ","combo_mask 1 70000\n"),line(good,"combo_mask 1 ","combo_mask 1 65535\n"),
        line(good,"combo_pad 1 ","combo_pad 1 -1\n"),line(good,"combo_pad 1 ","combo_pad 1 999\n"),
        line(good,"combo_name 1 ",""),line(good,"combo_name 1 ","combo_name 1 bad\tname\n"),
        line(good,"icon ammo_up ",""),line(good,"icon ammo_down ","")};
    for(const auto &value:bad) {
        write_text("touch_template_01.txt",value); char name[64]="stale";
        assert(!touch_template_info(0,name,sizeof name) && !name[0] && !touch_template_use(0));
        same_layout(g_s.layout,before); same_combos(cfg,before_cfg); assert(read_text(COMMON_PATH)==active && read_text("touch_template_01.txt")==value);
    }
    write_text("touch_template_01.txt",good); assert(touch_template_use(0));
    puts("PASS: empty, truncated, missing-field and invalid version/name/geometry/mask/pad templates fail closed without changing active layout/config or rewriting the bad input");
}
struct TestIcon { std::string bytes, hash, path; };
static TestIcon stored_icon(std::string bytes) {
    Sha256 sha; char hash[65]; sha256_init(&sha); sha256_update(&sha,bytes.data(),bytes.size()); sha256_hex(&sha,hash);
    assert(SDL_CreateDirectory("touch_icons"));
    TestIcon result{bytes,hash,std::string("touch_icons/")+hash+".rgba"};
    write_text(result.path.c_str(),bytes); return result;
}
static TestIcon test_icon(unsigned char seed,int width=8,int height=8) {
    std::string bytes("TICO",4);
    for(int dimension:{width,height}) { bytes+=(char)(dimension&255); bytes+=(char)(dimension>>8); }
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        bytes+=(char)((x+y)%2?seed:240); bytes+=(char)(255-seed); bytes+=(char)(seed/2); bytes+=(char)255;
    }
    return stored_icon(bytes);
}
static TestIcon shaped_icon(int shape) {
    const int width=shape==0?128:shape==1?32:96, height=shape==0?32:shape==1?128:96;
    std::string bytes("TICO",4);
    for(int dimension:{width,height}) { bytes+=(char)(dimension&255); bytes+=(char)(dimension>>8); }
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        float dx=2.f*(x+.5f)/width-1,dy=2.f*(y+.5f)/height-1;
        if(shape==1) std::swap(dx,dy);
        const bool inside=shape<2 ? fabsf(dy)<.2f || (dx>0 && fabsf(dy)<1-dx)
            : hypotf(dx,dy)<.65f+.3f*cosf(5*atan2f(dy,dx));
        bytes+=(char)(shape==0?60:shape==1?245:255);
        bytes+=(char)(shape==0?210:shape==1?115:205);
        bytes+=(char)(shape==0?245:shape==1?100:60);
        bytes+=(char)(inside?(x%8?255:128):0);
    }
    return stored_icon(bytes);
}
static void expect_icon(int button,int expected,const char *hash="") {
    int choice=999; char actual[65]="stale";
    assert(touch_icon_get(button,&choice,actual) && choice==expected && !strcmp(actual,hash));
}
static void set_icon(int button,int choice,const char *hash="") {
    const uint64_t generation=touch_layout_generation();
    assert(touch_icon_set(button,choice,hash,generation));
    assert(touch_layout_generation()>generation); expect_icon(button,choice,choice==-1?hash:"");
}
static void icon_template_isolation() {
    fresh(); enable_universal_combos(); cfg.volume=71; assert(save_layout_file(g_s.layout,COMMON_PATH));
    strcpy(cfg.touch_combo_name[0],"  蓄力  ");
    const Config original_cfg=cfg;
    assert(TOUCH_ICON_BUTTON_COUNT==25 && touch_icon_builtin_count()==15);
    const int original_controls[]={C_HEAVY,C_LIGHT,C_DODGE,C_SHEATHE,C_R,C_L,C_ZL,C_ZR,C_MENU,C_HMINUS,C_MAP,C_R3,
        C_HDPAD,C_DPAD_TOGGLE,C_STICK,C_SETTINGS,C_COMBO,C_COMBO+1,C_COMBO+2,C_COMBO+3,C_COMBO+4,C_COMBO+5};
    for(int button=0;button<22;++button) assert(ICON_CONTROLS[button]==original_controls[button]);
    assert(ICON_CONTROLS[22]==C_AMMO_UP && ICON_CONTROLS[23]==C_AMMO_DOWN);
    assert(ICON_CONTROLS[24]==C_LOCK);
    for(int i=1;i<=touch_icon_builtin_count();++i) assert(*touch_icon_builtin_name(i));
    assert(!*touch_icon_builtin_name(0) && !*touch_icon_builtin_name(999));
    for(int button=0;button<TOUCH_ICON_BUTTON_COUNT;++button) {
        assert(*touch_icon_button_name(button)); expect_icon(button,0);
        Layout expected=g_s.layout; set_icon(button,button%touch_icon_builtin_count()+1);
        expected.icons[ICON_CONTROLS[button]]=g_s.layout.icons[ICON_CONTROLS[button]];
        same_layout(g_s.layout,expected); same_combos(cfg,original_cfg); assert(cfg.volume==71);
    }
    const TestIcon a=test_icon(30),b=test_icon(150);
    assert(a.hash!=b.hash && a.path!=b.path);
    set_icon(4,-1,a.hash.c_str()); set_icon(16,3); set_icon(22,-1,a.hash.c_str()); set_icon(23,4); set_icon(24,15);
    const Layout layout_a=g_s.layout; assert(touch_template_save(0,"图标模板 A"));
    const std::string file_a=read_text("touch_template_01.txt");
    set_icon(4,-1,b.hash.c_str()); set_icon(16,12); set_icon(22,8); set_icon(23,-1,b.hash.c_str()); set_icon(24,-1,b.hash.c_str());
    const Layout layout_b=g_s.layout; assert(touch_template_save(1,"图标模板 B"));
    const std::string file_b=read_text("touch_template_02.txt");
    set_icon(4,2); // Active edits do not rewrite either saved template.
    for(int slot:{0,1,0,1}) {
        const uint64_t generation=touch_layout_generation(); assert(touch_template_use(slot));
        assert(touch_layout_generation()>generation); same_layout(g_s.layout,slot?layout_b:layout_a);
        expect_icon(4,-1,(slot?b:a).hash.c_str()); expect_icon(16,slot?12:3);
        expect_icon(22,slot?8:-1,slot?"":a.hash.c_str()); expect_icon(23,slot?-1:4,slot?b.hash.c_str():"");
        expect_icon(24,slot?-1:15,slot?b.hash.c_str():"");
        assert(read_text("touch_template_01.txt")==file_a && read_text("touch_template_02.txt")==file_b);
        assert(read_text(a.path.c_str())==a.bytes && read_text(b.path.c_str())==b.bytes);
    }
    touch_reset(); g_s={}; update(); same_layout(g_s.layout,layout_b);
    assert(file_a.rfind("layout_version 5",0)==0);
    const std::string v2=legacy_template(file_a,2);
    write_text("touch_template_03.txt",v2); assert(touch_template_use(2));
    Layout v2_expected=layout_a; v2_expected.icons[C_AMMO_UP]=v2_expected.icons[C_AMMO_DOWN]=v2_expected.icons[C_LOCK]={};
    same_layout(g_s.layout,v2_expected); same_combos(cfg,original_cfg);
    expect_icon(4,-1,a.hash.c_str()); expect_icon(22,0); expect_icon(23,0); expect_icon(24,0);
    assert(read_text("touch_template_03.txt")==v2);
    const std::string legacy=legacy_template(file_a,1);
    write_text("touch_template_04.txt",legacy); assert(touch_template_use(3));
    same_combos(cfg,original_cfg);
    Layout legacy_expected=layout_a;
    for(int c:ICON_CONTROLS) legacy_expected.icons[c]={};
    same_layout(g_s.layout,legacy_expected);
    for(int button=0;button<TOUCH_ICON_BUTTON_COUNT;++button) expect_icon(button,0);
    puts("PASS: all 25 icon controls preserve the original 0-21 identities, add ammo at 22/23 and lock at 24; 15 builtins and independent custom-image templates round-trip through edits/switch/reload; v5 stores all 25, v2 preserves its 22 and defaults ammo/lock, v1 defaults every icon while keeping exact combo names");
}
static void icon_import_failures() {
    fresh(); assert(save_layout_file(g_s.layout,COMMON_PATH)); const TestIcon good=test_icon(65);
    set_icon(4,-1,good.hash.c_str()); assert(touch_template_save(0,"导入基线"));
    const Layout before=g_s.layout; const std::string disk=read_text(COMMON_PATH),saved=read_text("touch_template_01.txt");
    auto unchanged=[&]() {
        same_layout(g_s.layout,before); assert(read_text(COMMON_PATH)==disk && read_text("touch_template_01.txt")==saved);
    };
    const uint64_t generation=touch_layout_generation();
    for(const std::string &hash:std::vector<std::string>{"","../outside", "D:/outside.rgba", "\\\\server\\share",std::string(64,'A'),std::string(63,'a'),std::string(65,'a'),std::string(64,'0')}) {
        assert(!touch_icon_set(4,-1,hash.c_str(),generation)); unchanged(); assert(touch_layout_generation()==generation);
    }
    const int invalid_buttons[]={-1,TOUCH_ICON_BUTTON_COUNT,999};
    for(int button:invalid_buttons) {
        assert(!touch_icon_get(button,nullptr,nullptr) && !touch_icon_set(button,1,"",generation)); unchanged();
    }
    for(int choice:{-2,touch_icon_builtin_count()+1}) { assert(!touch_icon_set(4,choice,"",generation)); unchanged(); }
    std::vector<std::string> corrupt={"TICO",good.bytes+'x',good.bytes.substr(0,good.bytes.size()-1)};
    std::string bad_magic=good.bytes; bad_magic[0]='X'; corrupt.push_back(bad_magic);
    std::string zero_width=good.bytes; zero_width[4]=zero_width[5]=0; corrupt.push_back(zero_width);
    std::string wide=good.bytes; wide[4]=(char)129; wide[5]=0; corrupt.push_back(wide);
    for(const std::string &bytes:corrupt) {
        const TestIcon invalid=stored_icon(bytes); std::vector<unsigned char> pixels; int w=0,h=0;
        assert(!load_icon_pixels(invalid.hash.c_str(),pixels,&w,&h) && pixels.empty());
        assert(!touch_icon_set(4,-1,invalid.hash.c_str(),generation)); unchanged();
    }
    std::string changed=good.bytes; changed.back()^=1; write_text(good.path.c_str(),changed);
    assert(!touch_icon_set(4,-1,good.hash.c_str(),generation)); unchanged(); write_text(good.path.c_str(),good.bytes);
    assert(SDL_CreateDirectory("touch_layout_common.txt.tmp"));
    assert(!touch_icon_set(4,1,"",generation)); unchanged(); assert(touch_layout_generation()==generation);
    assert(SDL_RemovePath("touch_layout_common.txt.tmp"));
    // A late import result from before a template switch or editor opening cannot change the current layout.
    assert(touch_template_use(0)); assert(!touch_icon_set(4,1,"",generation)); unchanged();
    const uint64_t editing_generation=touch_layout_generation(); touch_edit_begin(); update();
    assert(!touch_icon_set(4,1,"",editing_generation)); editor_click(E_CANCEL); update();
    assert(!touch_icon_set(4,1,"",editing_generation)); unchanged();
    for(const std::string &extra:std::vector<std::string>{"icon guard -1 ../outside\n","icon guard 999 -\n","icon guard 1 -\n"}) {
        write_text("touch_template_01.txt",saved+extra); char name[64];
        assert(!touch_template_info(0,name,sizeof name) && !touch_template_use(0)); same_layout(g_s.layout,before);
        assert(read_text(COMMON_PATH)==disk);
    }
    write_text("touch_template_01.txt",saved);
    puts("PASS: invalid buttons/choices, path-like or invalid hashes, missing/corrupt/bad-size/trailing image data, hash mismatch, failed writes, stale import generations and malformed template icons leave active state intact");
}
static void icon_resource_fallback() {
    fresh(); assert(save_layout_file(g_s.layout,COMMON_PATH));
    const TestIcon missing=test_icon(90),corrupt=test_icon(91),valid=test_icon(92,128,128);
    std::vector<unsigned char> pixels; int w=0,h=0;
    assert(load_icon_pixels(valid.hash.c_str(),pixels,&w,&h) && w==128 && h==128 && pixels.size()==128*128*4);
    set_icon(4,-1,missing.hash.c_str()); assert(touch_template_save(0,"资源缺失"));
    assert(SDL_RemovePath(missing.path.c_str())); assert(touch_template_use(0)); expect_icon(4,-1,missing.hash.c_str());
    set_icon(4,-1,corrupt.hash.c_str()); assert(touch_template_save(1,"资源损坏"));
    write_text(corrupt.path.c_str(),corrupt.bytes.substr(0,9)); assert(touch_template_use(1)); expect_icon(4,-1,corrupt.hash.c_str());
    for(int slot:{0,1}) {
        assert(touch_template_use(slot)); const Layout selected=g_s.layout;
        ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=ImVec2((float)screen_w,(float)screen_h); io.DeltaTime=1.0f/60;
        ImGui::NewFrame(); icon_textures_frame(); ImDrawList *dl=ImGui::GetBackgroundDrawList();
        assert(!custom_icon(dl,selected.icons[C_R],ImVec2(200,200),30,IM_COL32_WHITE,1));
        const int start=dl->VtxBuffer.Size;
        control(dl,ImGui::GetFont(),selected,C_R,(float)screen_w,(float)screen_h,g_s,1);
        const int count=dl->VtxBuffer.Size-start; Layout fallback=selected; fallback.icons[C_R]={};
        control(dl,ImGui::GetFont(),fallback,C_R,(float)screen_w,(float)screen_h,g_s,1);
        assert(count>0 && dl->VtxBuffer.Size-start==count*2);
        for(int i=0;i<count;++i) {
            const ImDrawVert &a=dl->VtxBuffer[start+i],&b=dl->VtxBuffer[start+count+i];
            assert(a.pos.x==b.pos.x && a.pos.y==b.pos.y && a.uv.x==b.uv.x && a.uv.y==b.uv.y && a.col==b.col);
        }
        ImGui::Render(); same_layout(g_s.layout,selected); // Rendering never erases a recoverable asset reference.
    }
    set_icon(4,-1,valid.hash.c_str());
    ImGui::NewFrame(); icon_textures_frame(); ImTextureData *texture=icon_texture(valid.hash.c_str());
    assert(texture && texture->Width==128 && texture->Height==128 && !memcmp(texture->Pixels,pixels.data(),pixels.size()));
    assert(custom_icon(ImGui::GetBackgroundDrawList(),g_s.layout.icons[C_R],ImVec2(200,200),30,IM_COL32_WHITE,1));
    ImGui::Render();
    puts("PASS: native image loader accepts the bounded 128px resource; absent/corrupt template resources retain their references and draw exactly the original R fallback; valid RGBA reaches the actual ImGui custom image texture");
}
static void freeform_icon_drawing() {
    const TestIcon images[]={shaped_icon(0),shaped_icon(1),shaped_icon(2)};
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) {
        fresh((int)size.x,(int)size.y); enable_universal_combos();
        for(const TestIcon &image:images) for(float scale:{.6f,1.7f}) for(int floating:{0,1}) {
            cfg.touch_floating=floating;
            ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=size; io.DeltaTime=1.f/60;
            ImGui::NewFrame(); icon_textures_frame();
            ImTextureData *tex=icon_texture(image.hash.c_str()); assert(tex);
            assert(image.bytes.size()==8+tex->Width*tex->Height*4);
            assert(!memcmp(tex->Pixels,image.bytes.data()+8,image.bytes.size()-8));
            bool transparent=false,partial=false,opaque=false;
            for(size_t i=11;i<image.bytes.size();i+=4) {
                const unsigned char alpha=image.bytes[i];
                transparent|=alpha==0; partial|=alpha==128; opaque|=alpha==255;
            }
            assert(transparent && partial && opaque);
            ImDrawList *dl=ImGui::GetBackgroundDrawList();
            for(int c:ICON_CONTROLS) for(int phase=0;phase<4;++phase) {
                State s=g_s; s.edit=phase==3; s.stick_on=phase==1 || phase==2; s.run=phase==2;
                for(Finger &f:s.f) f={};
                if(phase==1 || phase==2) s.f[0].kind=F_BUTTON,s.f[0].ctl=c,s.f[0].part=1;
                Layout l=s.layout; l.scale[c]=scale;
                l.icons[c].choice=-1; SDL_strlcpy(l.icons[c].hash,image.hash.c_str(),sizeof l.icons[c].hash);
                const Geo g=place(l,c,size.x,size.y);
                s.bx=g.c.x+7; s.by=g.c.y-9; s.kx=s.bx+g.r*.7f; s.ky=s.by-g.r*.4f;
                const int start=dl->VtxBuffer.Size,indices=dl->IdxBuffer.Size;
                control(dl,ImGui::GetFont(),l,c,size.x,size.y,s,.75f);
                if(c==C_STICK && floating && phase==0) {
                    assert(dl->VtxBuffer.Size==start && dl->IdxBuffer.Size==indices); continue;
                }
                // Exactly the complete image: no circular face, border, badge, label or running ring.
                assert(dl->VtxBuffer.Size-start==4 && dl->IdxBuffer.Size-indices==6);
                const bool moving=c==C_STICK && s.stick_on && !s.edit;
                const ImVec2 center=moving?ImVec2(s.kx,s.ky):g.c;
                const float radius=c==C_STICK?g.r*KNOB:g.r*((phase==1 || phase==2)?.95f:1);
                const float k=2*radius/std::max(tex->Width,tex->Height);
                const ImVec2 half(tex->Width*k*.5f,tex->Height*k*.5f);
                const float alpha=c==C_STICK && !moving?.75f*.8f:.75f;
                for(int v=0;v<4;++v) {
                    const bool right=v==1 || v==2,bottom=v>=2;
                    const ImDrawVert &vertex=dl->VtxBuffer[start+v];
                    assert(fabsf(vertex.pos.x-(center.x+(right?half.x:-half.x)))<.001f);
                    assert(fabsf(vertex.pos.y-(center.y+(bottom?half.y:-half.y)))<.001f);
                    assert(vertex.uv.x==(right?1.f:0.f) && vertex.uv.y==(bottom?1.f:0.f));
                    assert(vertex.col==fade(IM_COL32_WHITE,alpha));
                }
            }
            ImGui::Render();
        }
    }
    fresh();
    const TestIcon missing=test_icon(93); assert(SDL_RemovePath(missing.path.c_str()));
    ImGui::NewFrame(); icon_textures_frame(); ImDrawList *dl=ImGui::GetBackgroundDrawList();
    for(int choice:{0,1,-1}) {
        IconChoice icon_choice{}; icon_choice.choice=choice;
        if(choice==-1) SDL_strlcpy(icon_choice.hash,missing.hash.c_str(),sizeof icon_choice.hash);
        const int before=dl->VtxBuffer.Size;
        assert(!stick(dl,ImVec2(100,100),ImVec2(120,100),40,1,true,&icon_choice));
        assert(dl->VtxBuffer.Size-before>4);
        State s=g_s; s.stick_on=s.run=true; s.bx=s.by=100; s.kx=120; s.ky=100;
        Layout l=s.layout; l.icons[C_STICK]=icon_choice;
        const int start=dl->VtxBuffer.Size;
        control(dl,ImGui::GetFont(),l,C_STICK,1280,720,s,1);
        const int running=dl->VtxBuffer.Size-start; s.run=false;
        control(dl,ImGui::GetFont(),l,C_STICK,1280,720,s,1);
        assert(running>dl->VtxBuffer.Size-start-running);
        l.icons[C_R]=icon_choice;
        const int button=dl->VtxBuffer.Size;
        control(dl,ImGui::GetFont(),l,C_R,1280,720,s,1); assert(dl->VtxBuffer.Size-button>4);
    }
    ImGui::Render();
    puts("PASS: all 25 imported-image controls keep wide/tall/irregular RGBA shapes, full UVs and proportions without fixed discs/badges/labels at three resolutions and two scales, idle/pressed/running/editing and fixed/floating; defaults/builtins/missing-stick fallback retain native feedback");
}
static void auto_r_sources_and_ownership() {
    for(int floating:{0,1}) {
        fresh(); cfg.touch_auto_r=1; cfg.touch_floating=floating; drawn=0; update();
        const Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        const ImVec2 origin=floating?ImVec2(screen_w*.35f,screen_h*.75f):g.c;
        auto down=[&]() { event(SDL_EVENT_FINGER_DOWN,1,origin.x,origin.y); };
        auto move=[&](float radius) { event(SDL_EVENT_FINGER_MOTION,1,origin.x+radius*g.r,origin.y); };
        auto cancel=[&]() { event(SDL_EVENT_FINGER_CANCELED,1,origin.x,origin.y); };
        auto expect=[&](bool run,uint64_t touch=0,uint64_t physical=0) {
            const Input v=update(physical,physical!=0);
            assert(v.run==run && g_s.run==run);
            assert(v.touch_b==(touch|(run?B(BTN_R):0)) && v.b==(touch|physical|(run?B(BTN_R):0))); no_stick_camera(v);
        };
        down(); at(SDL_EVENT_FINGER_DOWN,C_R,2); move(1.7001f); expect(true,B(BTN_R));
        at(SDL_EVENT_FINGER_CANCELED,C_R,2); expect(true);
        at(SDL_EVENT_FINGER_DOWN,C_R,2); expect(true,B(BTN_R));
        move(1.4499f); expect(false,B(BTN_R));
        at(SDL_EVENT_FINGER_CANCELED,C_R,2); expect(false);
        strcpy(cfg.touch_weapon_combo[TOUCH_UNIVERSAL_PROFILE][0],"L+R");
        down(); move(1.7001f); at(SDL_EVENT_FINGER_DOWN,C_COMBO,3); expect(true,B(BTN_L)|B(BTN_R));
        at(SDL_EVENT_FINGER_CANCELED,C_COMBO,3); expect(true);
        at(SDL_EVENT_FINGER_DOWN,C_COMBO,3); expect(true,B(BTN_L)|B(BTN_R));
        move(1.4499f); expect(false,B(BTN_L)|B(BTN_R));
        at(SDL_EVENT_FINGER_CANCELED,C_COMBO,3); expect(false); cancel(); expect(false);
        down(); move(1.7001f); expect(true); cfg.touch_auto_r=0; expect(false);
        cfg.touch_auto_r=1; expect(true);
        move(1.4499f); expect(false);
        // A second finger while the first stick is active cannot own or duplicate the run.
        event(SDL_EVENT_FINGER_DOWN,2,origin.x+g.r*.25f,origin.y);
        const Finger *extra=finger(g_s,2); assert(!extra || (extra->kind!=F_STICK && extra->kind!=F_CAMERA));
        event(SDL_EVENT_FINGER_MOTION,2,origin.x+g.r*2,origin.y); expect(false);
        move(1.7001f); expect(true);
        event(SDL_EVENT_FINGER_CANCELED,2,origin.x+g.r*2,origin.y);
        cancel(); expect(false);
    }
    puts("PASS: rim AutoR marks native R as a touch source, preserves overlapping manual/combo R through either release order, follows live Off/On, and secondary stick touches cannot copy or steal the run");
}

static void auto_r_lifecycle() {
    for(int floating:{0,1}) for(int mode=0;mode<9;++mode) {
        fresh(); cfg.touch_auto_r=1; cfg.touch_floating=floating; drawn=0; update();
        if(mode==8) assert(touch_template_save(19,"AutoR lifecycle"));
        const Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        const ImVec2 origin=floating?ImVec2(screen_w*.35f,screen_h*.75f):g.c;
        event(SDL_EVENT_FINGER_DOWN,1,origin.x,origin.y);
        event(SDL_EVENT_FINGER_MOTION,1,origin.x+g.r*1.7f,origin.y);
        Input v=update(); assert(v.run && g_s.run && v.b==B(BTN_R) && v.touch_b==B(BTN_R)); no_stick_camera(v);
        switch(mode) {
            case 0: game_menu_state(true); break;
            case 1: settings_open=true; break;
            case 2: touch_edit_begin(); break;
            case 3: cfg.touch_controls=TOUCH_OFF; break;
            case 4: cfg.touch_show[TB_STICK]=0; break;
            case 5: break; // typing is passed to update below
            case 6: touch_reset(); break;
            case 7: in_quest=false; break;
            case 8: assert(touch_template_use(19)); break;
        }
        auto stopped=[&]() {
            const Input input=update(0,false,mode==5);
            if(input.run || g_s.run || ((input.b|input.touch_b)&B(BTN_R))) fprintf(stderr,"AUTOR LIFECYCLE FAILURE: floating=%d mode=%d run=%d b=%llx touch_b=%llx\n",floating,mode,input.run,(unsigned long long)input.b,(unsigned long long)input.touch_b);
            assert(!input.run && !g_s.run && !((input.b|input.touch_b)&B(BTN_R))); no_stick_camera(input);
        };
        stopped(); event(SDL_EVENT_FINGER_UP,1,screen_w+10000.f,-10000.f); stopped();
        if(mode==0) game_menu_state(false);
        settings_open=false; cfg.touch_controls=TOUCH_ON; cfg.touch_show[TB_STICK]=1;
        if(g_s.edit) { editor_click(E_CANCEL); update(); }
        v=update(); assert(!v.run && !g_s.run && !((v.b|v.touch_b)&B(BTN_R))); no_stick_camera(v);
    }
    puts("PASS: rim AutoR clears native and touch-source R in game menus, overlays, editor, Off, hidden stick, typing, reset and scene/template changes");
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

struct HunterArtCase { const char *name; void (*run)(); };
static const HunterArtCase hunter_art_cases[]={
    {"first-entry",hunter_art_first_entry},
    {"zone-lifetime",hunter_art_zone_lifetime},
    {"sparse-slots",hunter_art_sparse_slots},
    {"unknown-slots",hunter_art_unknown_slots},
    {"no-marker-input",hunter_art_no_marker_input},
    {"template-offsets",hunter_art_template_offsets},
    {"hidden-controls",hunter_art_hidden_controls},
    {"cancel-context",hunter_art_cancel_context},
    {"control-types",hunter_art_control_types},
};
static void run_hunter_art_cases(const char *selected) {
    int passed=0;
    for(const HunterArtCase &test:hunter_art_cases) {
        if(selected && strcmp(selected,test.name)) continue;
        // An assertion abort leaves its start record and stderr for the external wrapper.
        printf("{\"suite\":\"hunter-arts\",\"event\":\"case-start\",\"case\":\"%s\"}\n",test.name);
        test.run();
        printf("{\"suite\":\"hunter-arts\",\"event\":\"case-result\",\"case\":\"%s\",\"status\":\"pass\"}\n",test.name);
        ++passed;
    }
    assert(passed);
    printf("{\"suite\":\"hunter-arts\",\"event\":\"summary\",\"status\":\"pass\",\"passed\":%d,\"host_only\":true}\n",passed);
}
int main(int argc,char **argv) {
    bool arts_only=false;
    const char *art_case=nullptr;
    const char *const prefix="--hunter-arts-case=";
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--hunter-arts-only")) arts_only=true;
        else if(!strncmp(argv[i],prefix,strlen(prefix))) {
            art_case=argv[i]+strlen(prefix); arts_only=true;
        } else { fprintf(stderr,"Unknown argument: %s\n",argv[i]); return 2; }
    }
    if(art_case) {
        bool found=false;
        for(const HunterArtCase &test:hunter_art_cases) found|=!strcmp(art_case,test.name);
        if(!found) { fprintf(stderr,"Unknown Hunter Art case: %s\n",art_case); return 2; }
    }
    setvbuf(stdout,nullptr,_IONBF,0);
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS));
    window=SDL_CreateWindow("MHGU common touch host regression",screen_w,screen_h,SDL_WINDOW_HIDDEN); assert(window);
    ImGui::CreateContext(); ImGuiIO &io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr;
    io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures; io.Fonts->AddFontDefaultVector();
    if(arts_only) {
        run_hunter_art_cases(art_case);
        ImGui::DestroyContext(); SDL_DestroyWindow(window); SDL_Quit(); return 0;
    }
    common_startup_and_legacy(); ranged_guard_migration(); common_r_aim_identity(); ammo_default_migration(); common_controls_and_names();
    touch_button_sources(); item_gestures(); auto_r(); floating_stick();
    dpad_visibility(1280,720); dpad_visibility(640,360); right_stick_visibility(); layout_roundtrip(); show_settings();
    common_editor_and_failure(); template_roundtrip_and_failure(); template_corrupt_input(); auto_r_sources_and_ownership(); auto_r_lifecycle();
    icon_template_isolation(); icon_import_failures(); icon_resource_fallback(); freeform_icon_drawing();
    quest_dpad_camera(); hunter_art_first_entry(); universal_floating_stick(); universal_floating_boundaries(); universal_floating_priority_and_reset();
    common_floating_editor_roundtrip(); ordinary_button_drag(); button_drag_release_and_multitouch(); button_drag_lifecycle(); button_drag_exclusions();
    bow_charge_drag(); bow_drag_sources(); bow_drag_lifecycle(); bow_drag_manual_aim();
    editor_hud_scenes(); editor_hud_dragging(); editor_hud_feedback(); map_editable_scenes(); map_fixed_storage(); editor_toolbar_dragging(); hunter_art_zone_lifetime();
    hunter_art_sparse_slots(); hunter_art_unknown_slots(); hunter_art_no_marker_input(); hunter_art_template_offsets();
    hunter_art_hidden_controls(); hunter_art_cancel_context(); hunter_art_control_types(); editor_hud_priority_and_multitouch();
    ammo_repeat(); arrow_gesture_ownership(); ammo_visibility_and_editor(); arrow_gesture_lifecycle(); arrow_release_pending_and_overlap(); item_gesture_lifecycle();
    target_switch(1280,720); target_switch(2520,1080); target_switch(3840,1080); dedicated_lock_button(); persistent_target_lock();
    game_menu_taps(); game_menu_button_coexist(); game_menu_stick_coexist(); game_menu_gesture_lifecycle(); game_menu_pages_and_slider();
    game_menu_background_resume(); game_menu_mix_cancel_lifecycle();
    game_menu_mix_pause_finger(); game_menu_mix_pause_mapping(); game_menu_mix_pause_ownership();
    ImGui::DestroyContext();
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(2520,1080),ImVec2(720,1280)}) {
        fresh((int)size.x,(int)size.y); cfg.language=2; cfg.touch_controls=TOUCH_OFF; touch_opacity=0; update();
        g_gear_only=false; freeze_pause_region(); test_menu.mix_held=true;
        char path[100]; snprintf(path,sizeof path,"touch-combine-pause-hidden-%dx%d.png",screen_w,screen_h); preview(path);
    }
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080),ImVec2(3840,1080)}) {
        for(bool quest:{false,true}) {
            fresh((int)size.x,(int)size.y); in_quest=quest; cfg.language=2; cfg.touch_hud=1;
            enable_universal_combos();
            const char *const names[]={"蓄力", "回避组合", "锁定目标", "防御", "道具", "狩技三"};
            for(int i=0;i<TOUCH_COMBOS;++i) strcpy(cfg.touch_combo_name[i],names[i]);
            update(); char path[100];
            snprintf(path,sizeof path,"touch-common-%s-%dx%d.png",quest?"quest":"town",screen_w,screen_h); preview(path);
            touch_edit_begin(); update();
            snprintf(path,sizeof path,"touch-common-editor-%s-%dx%d.png",quest?"quest":"town",screen_w,screen_h); preview(path);
            editor_click(E_CANCEL); update();
        }
    }
    fresh(); cfg.touch_floating=1; update(); preview("touch-common-floating-idle-1280x720.png");
    event(SDL_EVENT_FINGER_DOWN,1,448,518); event(SDL_EVENT_FINGER_MOTION,1,488,493); update();
    preview("touch-common-floating-active-1280x720.png"); event(SDL_EVENT_FINGER_CANCELED,1,488,493); update();
    cfg.touch_floating=0; update(); preview("touch-common-fixed-1280x720.png");
    cfg.touch_floating=1; touch_edit_begin(); update(); preview("touch-common-floating-editor-1280x720.png"); editor_click(E_CANCEL); update();
    for(int floating:{0,1}) for(int enabled:{0,1}) {
        fresh(); cfg.language=2; cfg.touch_floating=floating; cfg.touch_auto_r=enabled; update();
        const Geo g=place(g_s.layout,C_STICK,(float)screen_w,(float)screen_h);
        const ImVec2 origin=floating?ImVec2(screen_w*.35f,screen_h*.75f):g.c;
        event(SDL_EVENT_FINGER_DOWN,1,origin.x,origin.y);
        event(SDL_EVENT_FINGER_MOTION,1,origin.x+1.7f*g.r,origin.y); const Input v=update();
        assert(v.run==(enabled!=0) && g_s.run==v.run && v.touch_b==(enabled?B(BTN_R):0) && v.b==(enabled?B(BTN_R):0));
        char path[100]; snprintf(path,sizeof path,"touch-auto-r-%s-%s-1280x720.png",floating?"floating":"fixed",enabled?"on":"off");
        preview(path); event(SDL_EVENT_FINGER_CANCELED,1,origin.x+g.r,origin.y); update();
    }
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) {
        fresh((int)size.x,(int)size.y); cfg.language=2; ranged=WEAPON_BOW; weapon_override=10; local_hunter=0x08004000;
        cfg.touch_show[TB_AMMO]=1; enable_universal_combos(); update(); char path[100];
        snprintf(path,sizeof path,"touch-common-ammo-combos-%dx%d.png",screen_w,screen_h); preview(path);
        touch_edit_begin(); update(); snprintf(path,sizeof path,"touch-common-ammo-editor-%dx%d.png",screen_w,screen_h); preview(path);
        editor_click(E_CANCEL); update();
    }
    for(const ImVec2 size:{ImVec2(640,360),ImVec2(1280,720),ImVec2(2520,1080)}) {
        fresh((int)size.x,(int)size.y); cfg.language=2; cfg.touch_floating=0;
        ranged=WEAPON_BOW; weapon_override=10; cfg.touch_show[TB_AMMO]=1; enable_universal_combos();
        const TestIcon shapes[]={shaped_icon(0),shaped_icon(1),shaped_icon(2)};
        for(int button:{0,2,4,14,16,22,23}) set_icon(button,-1,shapes[button%3].hash.c_str());
        update(); char path[100];
        snprintf(path,sizeof path,"touch-freeform-icons-%dx%d.png",screen_w,screen_h); preview(path);
        at(SDL_EVENT_FINGER_DOWN,C_R); at(SDL_EVENT_FINGER_DOWN,C_STICK,2);
        at(SDL_EVENT_FINGER_MOTION,C_STICK,2,1,0); cfg.touch_auto_r=1; update();
        snprintf(path,sizeof path,"touch-freeform-active-%dx%d.png",screen_w,screen_h); preview(path);
        event(SDL_EVENT_FINGER_CANCELED,1,0,0); event(SDL_EVENT_FINGER_CANCELED,2,0,0); update();
        touch_edit_begin(); update();
        snprintf(path,sizeof path,"touch-freeform-editor-%dx%d.png",screen_w,screen_h); preview(path);
        editor_click(E_CANCEL); update();
    }
    struct MapPreview { int w,h; bool quest; float x,y; const char *name; };
    const MapPreview maps[]={
        {1280,720,false,.5f,0,"touch-map-editor-town-1280x720.png"},
        {2520,1080,true,1,1,"touch-map-editor-quest-2520x1080.png"},
        {640,360,true,.5f,1,"touch-map-editor-moved-640x360.png"},
        {720,1280,false,.5f,0,"touch-map-editor-portrait-720x1280.png"},
    };
    for(const MapPreview &test:maps) {
        fresh(test.w,test.h); in_quest=test.quest; cfg.language=2; cfg.touch_hud=1; enable_universal_combos(); update();
        touch_edit_begin(); update(); editor_move_panel(test.x,test.y);
        assert(editor_hud_rect(TZ_MAP).ok);
        preview(test.name); editor_click(E_CANCEL); update();
    }
    SDL_DestroyWindow(window); SDL_Quit();
    puts("PASS: all common layout/template and retained touch control regressions (host stubs, no APK/game execution)"); return 0;
}
