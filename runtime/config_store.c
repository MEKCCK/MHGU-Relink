// Atomic INI persistence and live settings. Cold boot state is never mutated;
// each consumer reads hot values atomically on its own thread.
#include "config.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static Config saved, startup;
static bool have_saved, runtime_ready, runtime_fsr;
static SDL_AtomicInt volume = {100}, confirm_exit = {1}, language = {0};
static SDL_AtomicInt live_fps, live_fov, live_mouse, live_fxaa = {1}, live_msaa = {1},
                     live_vsync = {1}, live_face, live_menu_confirm, live_background = {1};
static void set_float(SDL_AtomicInt *slot, float value) {
    int bits; _Static_assert(sizeof bits == sizeof value, "32-bit float settings");
    memcpy(&bits, &value, sizeof bits); SDL_SetAtomicInt(slot, bits);
}
static float get_float(SDL_AtomicInt *slot) {
    int bits = SDL_GetAtomicInt(slot); float value; memcpy(&value, &bits, sizeof value); return value;
}
// A save updates changed hot fields only. Saving the volume must not silently
// remove an unchanged FOV/FPS override supplied by the development environment.
static void publish_live(const Config *c, bool all) {
    if (all || c->fps != saved.fps) set_float(&live_fps, (float)c->fps);
    if (all || c->fov != saved.fov) set_float(&live_fov, c->fov);
    if (all || c->mouse_sensitivity != saved.mouse_sensitivity) set_float(&live_mouse, c->mouse_sensitivity);
    if (all || c->fxaa != saved.fxaa) SDL_SetAtomicInt(&live_fxaa, c->fxaa);
    if (all || c->msaa != saved.msaa) SDL_SetAtomicInt(&live_msaa, c->msaa);
    if (all || c->vsync != saved.vsync) SDL_SetAtomicInt(&live_vsync, c->vsync);
    if (all || c->face_buttons != saved.face_buttons) SDL_SetAtomicInt(&live_face, c->face_buttons);
    if (all || c->menu_confirm != saved.menu_confirm) SDL_SetAtomicInt(&live_menu_confirm, c->menu_confirm);
    if (all || c->background_input != saved.background_input) SDL_SetAtomicInt(&live_background, c->background_input);
}
void config_store_loaded(const Config *c) {
    publish_live(c, !have_saved);
    saved = *c; have_saved = true;
    SDL_SetAtomicInt(&language, c->language);
    SDL_SetAtomicInt(&volume, c->volume);
    SDL_SetAtomicInt(&confirm_exit, c->confirm_exit);
}
void config_runtime_init(const Config *effective) {
    startup = saved; runtime_ready = true; runtime_fsr = effective->fsr != 0;
    publish_live(effective, true);
    const char *override = getenv("MHGU_FRAMERATE");
    if (override && *override) {
        float f = strtof(override, NULL);
        if (!(f >= 30)) f = 30;
        if (f > 240) f = 240;
        set_float(&live_fps, f);
    }
}
float config_live_fps(void) { return get_float(&live_fps); }
float config_live_fov(void) { return get_float(&live_fov); }
float config_live_mouse_sensitivity(void) { return get_float(&live_mouse); }
int config_live_fxaa(void) { return runtime_fsr ? 0 : SDL_GetAtomicInt(&live_fxaa); }
int config_live_msaa(void) { return runtime_fsr ? 1 : SDL_GetAtomicInt(&live_msaa); }
int config_live_vsync(void) { return SDL_GetAtomicInt(&live_vsync); }
int config_live_menu_confirm(void) { return SDL_GetAtomicInt(&live_menu_confirm); }
int config_live_face_buttons(void) { return SDL_GetAtomicInt(&live_face); }
int config_live_background_input(void) { return SDL_GetAtomicInt(&live_background); }
int config_language(void) { return SDL_GetAtomicInt(&language); }
Config config_editable(void) { return saved; }
float config_audio_gain(void) { return SDL_GetAtomicInt(&volume) / 100.0f; }
bool config_confirm_exit(void) { return SDL_GetAtomicInt(&confirm_exit) != 0; }
bool config_restart_required(const Config *c) {
    if (!runtime_ready) return false;
    if (c->hp.quests < 0 || c->hp.quests > HP_QUESTS_MAX || !memchr(c->ldn_join, 0, sizeof c->ldn_join)) return true;
    return c->fsr != startup.fsr || c->fsr_upscaler != startup.fsr_upscaler ||
        c->fsr_quality != startup.fsr_quality || c->fsr_sharpness != startup.fsr_sharpness ||
        c->fsr_objmotion != startup.fsr_objmotion || c->fsr_reactive != startup.fsr_reactive ||
        c->fsr_animsurf != startup.fsr_animsurf || c->display_mode != startup.display_mode || c->width != startup.width || c->height != startup.height ||
        c->render_w != startup.render_w || c->render_h != startup.render_h || c->aspect_ratio != startup.aspect_ratio ||
        c->ui_anchor != startup.ui_anchor || c->ldn_port != startup.ldn_port || strcmp(c->ldn_join, startup.ldn_join) ||
        c->sync_small_monsters != startup.sync_small_monsters || c->village_in_hub != startup.village_in_hub ||
        c->bonus_content != startup.bonus_content || c->hp.quests != startup.hp.quests || c->hp.village != startup.hp.village ||
        memcmp(c->hp.scale, startup.hp.scale, sizeof c->hp.scale) ||
        memcmp(c->hp.quest, startup.hp.quest, sizeof c->hp.quest[0] * (size_t)c->hp.quests);
}
bool config_field_restart(const char *field) {
    static const char *const fields[] = {"Window mode", "Screen resolution", "Render resolution", "Automatic aspect ratio",
        "Aspect ratio", "HUD placement", "Host port (TCP / UDP)", "Join addresses", "Synchronize small monsters",
        "Village quests at the Hub counter", "1 / 2 / 3 / 4 hunters", "Village quest HP in Hub", "Quest ID", "Hunter factors",
        "Add quest override", "Remove", "Import missing bonus content from donor save",
        "FSR", "Upscaler", "FSR quality", "Sharpness", "Object motion", "Reactive mask", "Animated surfaces"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) if (!strcmp(field, fields[i])) return true;
    return false;
}

static bool fail(char *error, size_t size, const char *message) {
    if (size) snprintf(error, size, "%s", message);
    return false;
}
static bool size_valid(int w, int h) {
    return (!w && !h) || (w >= 320 && w <= 16384 && h >= 180 && h <= 16384);
}
static bool factor_valid(float f) { return isfinite(f) && f >= HP_SCALE_MIN && f <= HP_SCALE_MAX; }
bool config_validate(const Config *c, char *error, size_t size) {
    if (c->fsr_upscaler < FSR_UP_3 || c->fsr_upscaler > FSR_UP_4 ||
        c->fsr_quality < FSR_NATIVE || c->fsr_quality > FSR_ULTRA_PERFORMANCE ||
        !isfinite(c->fsr_sharpness) || c->fsr_sharpness < 0 || c->fsr_sharpness > 1 ||
        !isfinite(c->fsr_reactive) || c->fsr_reactive < 0 || c->fsr_reactive > 1)
        return fail(error, size, "Invalid FSR quality, provider, sharpness or reactive mask.");
    if (c->menu_confirm < MENU_CONFIRM_ORIGINAL || c->menu_confirm > MENU_CONFIRM_EAST) return fail(error, size, "Invalid menu confirmation policy.");
    if (c->menu_scale < 100 || c->menu_scale > 200) return fail(error, size, "Invalid interface scale.");
    if (c->button_icons < 0 || c->button_icons > 2) return fail(error, size, "Invalid button icon layout.");
    if (c->language < 0 || c->language > 1) return fail(error, size, "Invalid menu language.");
    if (c->display_mode < DISPLAY_WINDOWED || c->display_mode > DISPLAY_BORDERLESS ||
        !size_valid(c->width, c->height) || !size_valid(c->render_w, c->render_h))
        return fail(error, size, "Resolution must be Desktop/Auto or 320x180 to 16384x16384.");
    if (!isfinite(c->aspect_ratio) || (c->aspect_ratio != 0 && (c->aspect_ratio < 0.5f || c->aspect_ratio > 10)))
        return fail(error, size, "Aspect ratio must be Auto or between 0.5 and 10.");
    if (!isfinite(c->fov) || c->fov < 0.5f || c->fov > 2 ||
        !isfinite(c->mouse_sensitivity) || c->mouse_sensitivity < 0.1f || c->mouse_sensitivity > 10)
        return fail(error, size, "FOV must be 0.5-2.0 and mouse sensitivity 0.1-10.");
    if (c->fps < 30 || c->fps > 240 || (c->msaa != 1 && c->msaa != 2 && c->msaa != 4 && c->msaa != 8) ||
        c->face_buttons < FACE_POSITION || c->face_buttons > FACE_LABEL ||
        c->ui_anchor < UI_CENTER || c->ui_anchor > UI_EDGES || c->volume < 0 || c->volume > 100)
        return fail(error, size, "Invalid frame cap, anti-aliasing, controller, HUD or volume setting.");
    const int toggles[] = {c->vsync, c->fxaa, c->background_input, c->sync_small_monsters,
                          c->village_in_hub, c->bonus_content, c->confirm_exit, c->fsr, c->fsr_objmotion, c->fsr_animsurf};
    for (size_t i = 0; i < sizeof toggles / sizeof *toggles; i++)
        if (toggles[i] != 0 && toggles[i] != 1) return fail(error, size, "Invalid On/Off value.");
    if (c->ldn_port < 1 || c->ldn_port > 65535 || !memchr(c->ldn_join, 0, sizeof c->ldn_join) ||
        strpbrk(c->ldn_join, "\r\n;#"))
        return fail(error, size, "Invalid port or Join address list (no newlines, ';' or '#').");
    if (!factor_valid(c->hp.village) || c->hp.quests < 0 || c->hp.quests > HP_QUESTS_MAX)
        return fail(error, size, "Invalid village HP or quest override count.");
    for (int j = 0; j < 4; j++)
        if (!factor_valid(c->hp.scale[j])) return fail(error, size, "HP factors must be 0.05-20.");
    for (int i = 0; i < c->hp.quests; i++) {
        if (!c->hp.quest[i].quest) return fail(error, size, "Quest IDs must be greater than zero.");
        for (int j = 0; j < i; j++)
            if (c->hp.quest[j].quest == c->hp.quest[i].quest) return fail(error, size, "Duplicate quest ID.");
        for (int j = 0; j < 4; j++)
            if (!factor_valid(c->hp.quest[i].scale[j])) return fail(error, size, "Quest HP factors must be 0.05-20.");
    }
    if (size) *error = 0;
    return true;
}

typedef struct { const char *section; char key[64], value[256]; bool seen; } Entry;
#define ENTRY_MAX (HP_QUESTS_MAX + 48)
static void add(Entry *entries, int *n, const char *section, const char *key, const char *value) {
    Entry *e = &entries[(*n)++];
    e->section = section;
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->value, sizeof e->value, "%s", value);
    e->seen = false;
}
static void add_int(Entry *e, int *n, const char *s, const char *k, int v) {
    char b[64]; snprintf(b, sizeof b, "%d", v); add(e, n, s, k, b);
}
static void add_float(Entry *e, int *n, const char *s, const char *k, float v) {
    char b[64]; snprintf(b, sizeof b, "%.9g", (double)v); add(e, n, s, k, b);
}
static void add_size(Entry *e, int *n, const char *k, int w, int h, const char *automatic) {
    char b[64]; snprintf(b, sizeof b, "%dx%d", w, h); add(e, n, "Display", k, w ? b : automatic);
}
static void normalize(const char *s, char *out, size_t size) {
    size_t n = 0;
    for (; *s && n + 1 < size; s++)
        if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') out[n++] = (char)tolower((unsigned char)*s);
    out[n] = 0;
}
static void flush_section(FILE *out, Entry *entries, int n, const char *section, const char *newline) {
    for (int i = 0; i < n; i++)
        if (!entries[i].seen && !strcasecmp(entries[i].section, section)) {
            fprintf(out, "%s = %s%s", entries[i].key, entries[i].value, newline);
            entries[i].seen = true;
        }
}
bool config_save(const Config *c, char *error, size_t error_size) {
    if (!config_validate(c, error, error_size)) return false;
    Entry entries[ENTRY_MAX]; int n = 0;
    static const char *const modes[] = {"Windowed", "Fullscreen", "Borderless"};
    static const char *const anchors[] = {"", "Center", "21:9", "32:9", "Edges"};
    add(entries, &n, "Display", "Mode", modes[c->display_mode]);
    add_size(entries, &n, "Resolution", c->width, c->height, "Desktop");
    add_size(entries, &n, "RenderResolution", c->render_w, c->render_h, "Auto");
    if (c->aspect_ratio == 0) add(entries, &n, "Display", "AspectRatio", "Auto");
    else add_float(entries, &n, "Display", "AspectRatio", c->aspect_ratio);
    add_float(entries, &n, "Display", "FOV", c->fov);
    add(entries, &n, "Display", "UIAnchor", anchors[c->ui_anchor]);
    char msaa[8]; snprintf(msaa, sizeof msaa, "%dx", c->msaa);
    add(entries, &n, "Anti-aliasing", "MSAA", c->msaa == 1 ? "Off" : msaa);
#define TOGGLE(section, key, field) add(entries, &n, section, key, c->field ? "On" : "Off")
    TOGGLE("Anti-aliasing", "FXAA", fxaa);
    TOGGLE("Upscaler", "FSR", fsr);
    add(entries, &n, "Upscaler", "Upscaler", c->fsr_upscaler == FSR_UP_4 ? "FSR4" : "FSR3");
    static const char *const quality[] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};
    add(entries, &n, "Upscaler", "FSRQuality", quality[c->fsr_quality]);
    add_float(entries, &n, "Upscaler", "Sharpness", c->fsr_sharpness);
    TOGGLE("Upscaler", "ObjectMotion", fsr_objmotion);
    add_float(entries, &n, "Upscaler", "Reactive", c->fsr_reactive);
    TOGGLE("Upscaler", "AnimSurf", fsr_animsurf);
    add_int(entries, &n, "Performance", "FPS", c->fps);
    TOGGLE("Performance", "VSync", vsync);
    add(entries, &n, "Controls", "FaceButtons", c->face_buttons == FACE_POSITION ? "Position" : "Label");
    TOGGLE("Controls", "BackgroundInput", background_input);
    add_float(entries, &n, "Mouse", "MouseSensitivity", c->mouse_sensitivity);
    add_int(entries, &n, "Multiplayer", "Port", c->ldn_port);
    add(entries, &n, "Multiplayer", "Join", c->ldn_join);
    TOGGLE("Multiplayer", "SyncSmallMonsters", sync_small_monsters);
    for (int i = 0; i < 4; i++) {
        char key[24]; snprintf(key, sizeof key, "%dPlayer%s", i + 1, i ? "s" : "");
        add_float(entries, &n, "Monster HP", key, c->hp.scale[i]);
    }
    for (int i = 0; i < c->hp.quests; i++) {
        char key[64], value[256]; snprintf(key, sizeof key, "Quest%u", c->hp.quest[i].quest);
        const float *v = c->hp.quest[i].scale;
        snprintf(value, sizeof value, "%.9g, %.9g, %.9g, %.9g", (double)v[0], (double)v[1], (double)v[2], (double)v[3]);
        add(entries, &n, "Monster HP", key, value);
    }
    TOGGLE("Village Quests", "VillageQuestsInHub", village_in_hub);
    add_float(entries, &n, "Village Quests", "HubHP", c->hp.village);
    TOGGLE("Bonus Content", "BonusContent", bonus_content);
    add_int(entries, &n, "Audio", "Volume", c->volume);
    TOGGLE("Application", "ConfirmExit", confirm_exit);
    static const char *const icons[] = {"Xbox", "Switch", "PlayStation"};
    static const char *const confirm[] = {"Original", "South", "East"};
    add(entries, &n, "Controls", "MenuConfirm", confirm[c->menu_confirm]);
    add_int(entries, &n, "Application", "MenuScale", c->menu_scale);
    add(entries, &n, "Application", "ButtonIcons", icons[c->button_icons]);
    add(entries, &n, "Application", "Language", c->language ? "Spanish" : "English");
#undef TOGGLE
    const char *path = getenv("MHGU_CONFIG");
    if (!path || !*path) path = "mhgurecomp.ini";
    char temp[1200];
    if (snprintf(temp, sizeof temp, "%s.settings-%llu.tmp", path, (unsigned long long)SDL_GetTicksNS()) >= (int)sizeof temp)
        return fail(error, error_size, "Settings path is too long.");
    FILE *in = fopen(path, "rb");
    if (!in && errno != ENOENT) return fail(error, error_size, "Cannot read the existing settings file.");
    FILE *out = fopen(temp, "wbx");
    if (!out) { if (in) fclose(in); return fail(error, error_size, "Cannot create a temporary settings file."); }
    char line[4096], section[128] = "";
    const char *newline = "\n";
    bool ok = true, ended_line = true;
    while (in && fgets(line, sizeof line, in)) {
        size_t len = strlen(line);
        if (!strchr(line, '\n') && !feof(in)) { ok = false; break; } // never truncate an unknown line
        if (strstr(line, "\r\n")) newline = "\r\n";
        char *s = line; while (isspace((unsigned char)*s)) s++;
        if (*s == '[' && strchr(s, ']')) {
            flush_section(out, entries, n, section, newline);
            snprintf(section, sizeof section, "%.*s", (int)(strchr(s, ']') - s - 1), s + 1);
        }
        char *eq = (*s && *s != ';' && *s != '#' && *s != '[') ? strchr(s, '=') : NULL;
        bool handled = false;
        if (eq) {
            char key[128], normalized[128];
            snprintf(key, sizeof key, "%.*s", (int)(eq - s), s); normalize(key, normalized, sizeof normalized);
            if (!strcmp(normalized, "framerate")) strcpy(normalized, "fps");
            for (int i = 0; i < n; i++) {
                char other[128]; normalize(entries[i].key, other, sizeof other);
                if (strcmp(normalized, other)) continue;
                const char *comment = strpbrk(eq + 1, ";#");
                if (comment) fprintf(out, "%.*s %s %s", (int)(eq - line + 1), line, entries[i].value, comment);
                else fprintf(out, "%.*s %s%s", (int)(eq - line + 1), line, entries[i].value, newline);
                entries[i].seen = true; handled = true; ended_line = true; break;
            }
            // Removing an override must remove its old value, including duplicate entries.
            if (!handled && !strncmp(normalized, "quest", 5) && isdigit((unsigned char)normalized[5])) handled = true;
        }
        if (!handled) { fputs(line, out); ended_line = len && line[len - 1] == '\n'; }
    }
    if (in) { if (ferror(in)) ok = false; fclose(in); }
    if (!ended_line) fputs(newline, out);
    flush_section(out, entries, n, section, newline);
    for (int i = 0; i < n; i++) if (!entries[i].seen) {
        fprintf(out, "%s[%s]%s", newline, entries[i].section, newline);
        flush_section(out, entries, n, entries[i].section, newline);
    }
    if (fflush(out) || ferror(out)) ok = false;
#ifdef _WIN32
    if (_commit(_fileno(out))) ok = false;
#else
    if (fsync(fileno(out))) ok = false;
#endif
    if (fclose(out)) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(temp, path) == 0;
#endif
    }
    if (!ok) { remove(temp); return fail(error, error_size, "Could not save settings; the previous file was kept."); }
    config_store_loaded(c);
    return true;
}
