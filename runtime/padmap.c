// Any controller, button by button. SDL only reads a controller as a gamepad if it knows its layout, and for many (old
// DirectInput ones, cheap Bluetooth ones, arcade sticks) it does not, or guesses wrong. Here the player presses, one by
// one, what each of the Switch's buttons should be; that makes an ordinary SDL mapping, which SDL then applies to every
// controller of that kind, and from there on it is a controller like any other for the rest of the port.
// (the settings menu: Controls, Map a controller; kept in gamepad_mappings.txt beside mhgurecomp.ini)
//
// SDL's own names are by position: "a" is the bottom face button, "b" the right one. The Switch's A is the right one,
// so A here goes to "b", and a mapped controller is always read by position (host_input.c: padmap_custom).
//
// It starts with the player moving every stick and trigger once and letting go: a trigger rests at one end of its axis
// but a controller reports that only from its first movement, and what rests where has to be known to tell a press.
#include "padmap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define FILE_NAME "gamepad_mappings.txt"
#define WATCH_NS 1500000000ull  // the menu has not looked for this long: its pages were left
#define LIST_NS 500000000ull
#define PRESS 14000             // of 32768: an axis this far from rest
#define RELEASED 6000
#define MAX_AXES 32
#define MAX_HATS 8
#define MAX_BUTTONS 64
#define TRIGGER_REST 20000      // an axis resting beyond this runs from one end to the other, as a trigger's
#define INPUT_TEXT 16
#define GUID_TEXT 33            // (SDL_GUIDToString: 32 digits)

static const struct { const char *field, *title; } STEPS[] = {
    {"b", "A (right button: confirm, special attack)"},
    {"a", "B (bottom button: cancel, dodge)"},
    {"y", "X (top button: attack)"},
    {"x", "Y (left button: use item, sheathe)"},
    {"leftshoulder", "L (camera)"},
    {"rightshoulder", "R (guard, run)"},
    {"lefttrigger", "ZL"},
    {"righttrigger", "ZR"},
    {"start", "Plus (the game's menu)"},
    {"back", "Minus"},
    {"dpup", "D-pad up"},
    {"dpdown", "D-pad down"},
    {"dpleft", "D-pad left"},
    {"dpright", "D-pad right"},
    {"-lefty", "Left stick up"},
    {"+lefty", "Left stick down"},
    {"-leftx", "Left stick left"},
    {"+leftx", "Left stick right"},
    {"leftstick", "Left stick click"},
    {"-righty", "Right stick up"},
    {"+righty", "Right stick down"},
    {"-rightx", "Right stick left"},
    {"+rightx", "Right stick right"},
    {"rightstick", "Right stick click"},
};
#define STEP_COUNT ((int)(sizeof STEPS / sizeof *STEPS))

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t watched;                      // padmap_watch's last call
    int count;                             // the list
    SDL_JoystickID ids[PADMAP_MAX];
    char names[PADMAP_MAX][64];
    bool custom[PADMAP_MAX];
    int begin, forget;                     // requests, index + 1
    bool cancel, save;
    bool active;                           // a controller is being mapped
    SDL_Joystick *joy;
    int step;
    bool held;                             // the last press has yet to be let go
    int naxes, nhats, nbuttons;
    Sint16 axis_rest[MAX_AXES];
    Uint8 hat_rest[MAX_HATS];
    char input[sizeof STEPS / sizeof *STEPS][INPUT_TEXT];
} P;
static char g_saved[PADMAP_MAX * 4][GUID_TEXT];  // those mapped by the player
static int g_nsaved;

// ---- the file: one SDL mapping a line, the first field the controller's GUID
static bool guid_saved(const char *guid) {
    for (int i = 0; i < g_nsaved; i++)
        if (!strcmp(g_saved[i], guid)) return true;
    return false;
}

static void remember(const char *guid) {
    if (!guid_saved(guid) && g_nsaved < (int)(sizeof g_saved / sizeof *g_saved))
        snprintf(g_saved[g_nsaved++], GUID_TEXT, "%s", guid);
}

// file without the line of guid, then `line` if any
static void file_put(const char *guid, const char *line) {
    FILE *f = fopen(FILE_NAME, "r"), *o = fopen(FILE_NAME ".tmp", "w");
    if (!o) { LOG("[padmap] can't write %s\n", FILE_NAME); if (f) fclose(f); return; }
    if (f) {
        char l[1024];
        size_t n = strlen(guid);
        while (fgets(l, sizeof l, f))
            if (strncmp(l, guid, n) || l[n] != ',') fputs(l, o);
        fclose(f);
    }
    if (line) fprintf(o, "%s\n", line);
    fclose(o);
    remove(FILE_NAME);
    rename(FILE_NAME ".tmp", FILE_NAME);
}

void padmap_init(void) {
    FILE *f = fopen(FILE_NAME, "r");
    if (!f) return;
    char l[1024];
    int n = 0;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *comma = strchr(l, ',');
        if (!comma || SDL_AddGamepadMapping(l) < 0) continue;
        *comma = 0;
        remember(l);
        n++;
    }
    fclose(f);
    LOG("[padmap] %d mapped controller(s)\n", n);
}

bool padmap_custom(SDL_JoystickID id) {
    char guid[GUID_TEXT];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    return guid_saved(guid);
}

// ---- reading it
static void snapshot(void) {
    for (int i = 0; i < P.naxes; i++) P.axis_rest[i] = SDL_GetJoystickAxis(P.joy, i);
    for (int i = 0; i < P.nhats; i++) P.hat_rest[i] = SDL_GetJoystickHat(P.joy, i);
}

static bool all_released(void) {
    for (int i = 0; i < P.nbuttons; i++)
        if (SDL_GetJoystickButton(P.joy, i)) return false;
    for (int i = 0; i < P.nhats; i++)
        if (SDL_GetJoystickHat(P.joy, i) != P.hat_rest[i]) return false;
    for (int i = 0; i < P.naxes; i++)
        if (abs(SDL_GetJoystickAxis(P.joy, i) - P.axis_rest[i]) > RELEASED) return false;
    return true;
}

static bool is_trigger(int step) { return !strcmp(STEPS[step].field, "lefttrigger") || !strcmp(STEPS[step].field, "righttrigger"); }

// what is pressed now, in a mapping's terms: b3, h0.1, +a2... (the strongest axis, if nothing else); false: nothing
static bool read_input(int step, char *out) {
    for (int i = 0; i < P.nbuttons; i++)
        if (SDL_GetJoystickButton(P.joy, i)) { snprintf(out, INPUT_TEXT, "b%d", i); return true; }
    for (int i = 0; i < P.nhats; i++) {
        Uint8 v = SDL_GetJoystickHat(P.joy, i) & ~P.hat_rest[i];
        static const Uint8 dirs[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
        for (int d = 0; d < 4; d++)
            if (v & dirs[d]) { snprintf(out, INPUT_TEXT, "h%d.%d", i, dirs[d]); return true; }
    }
    int best = -1, far = PRESS;
    for (int i = 0; i < P.naxes; i++) {
        int d = abs(SDL_GetJoystickAxis(P.joy, i) - P.axis_rest[i]);
        if (d > far) far = d, best = i;
    }
    if (best < 0) return false;
    int rest = P.axis_rest[best], now = SDL_GetJoystickAxis(P.joy, best);
    if (is_trigger(step) && rest < -TRIGGER_REST) snprintf(out, INPUT_TEXT, "a%d", best);
    else if (is_trigger(step) && rest > TRIGGER_REST) snprintf(out, INPUT_TEXT, "a%d~", best);
    else snprintf(out, INPUT_TEXT, "%ca%d", now > rest ? '+' : '-', best);
    return true;
}

static bool used(const char *input, int upto) {  // (a press already taken: a noisy axis, a double press)
    for (int i = 0; i < upto; i++)
        if (!strcmp(P.input[i], input)) return true;
    return false;
}

static void stop(void) {
    if (P.joy) SDL_CloseJoystick(P.joy);
    P.joy = NULL, P.active = false, P.held = false;
    P.cancel = P.save = false;
}

static void apply(void) {
    char guid[GUID_TEXT], name[64], line[1024];
    SDL_GUIDToString(SDL_GetJoystickGUID(P.joy), guid, sizeof guid);
    snprintf(name, sizeof name, "%s", SDL_GetJoystickName(P.joy) ? SDL_GetJoystickName(P.joy) : "Controller");
    for (char *c = name; *c; c++)
        if (*c == ',' || *c == '\r' || *c == '\n') *c = ' ';
    int n = snprintf(line, sizeof line, "%s,%s,", guid, name);
    for (int i = 0; i < STEP_COUNT; i++)
        if (P.input[i][0]) n += snprintf(line + n, sizeof line - (size_t)n, "%s:%s,", STEPS[i].field, P.input[i]);
    snprintf(line + n, sizeof line - (size_t)n, "platform:%s,", SDL_GetPlatform());
    int r = SDL_AddGamepadMapping(line);
    LOG("[padmap] %s: %s (%s)\n", name, line, r < 0 ? SDL_GetError() : "in use");
    if (r < 0) return;
    file_put(guid, line);
    remember(guid);
}

static void refresh_list(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    P.count = 0;
    for (int i = 0; i < n && P.count < PADMAP_MAX; i++) {
        const char *name = SDL_GetJoystickNameForID(ids[i]);
        // (not controllers, though Android lists them: runtime/host_input.c says the same of gamepads)
        static const char *const not_pads[] = {"qwerty", "virtual", "gpio", "keypad", "consumer control"};
        bool skip = false;
        for (unsigned k = 0; name && k < sizeof not_pads / sizeof *not_pads; k++)
            skip |= SDL_strcasestr(name, not_pads[k]) != NULL;
        if (skip) continue;
        P.ids[P.count] = ids[i];
        snprintf(P.names[P.count], 64, "%s", name ? name : "?");
        P.custom[P.count++] = padmap_custom(ids[i]);
    }
    SDL_free(ids);
}

void padmap_poll(void) {
    static uint64_t next_list;
    pthread_mutex_lock(&g_mu);
    uint64_t now = plat_now_ns();
    bool watched = P.watched && now - P.watched < WATCH_NS;
    if (watched && now >= next_list) refresh_list(), next_list = now + LIST_NS;
    if (P.forget) {
        int i = P.forget - 1;
        P.forget = 0;
        if (i < P.count) {
            char guid[GUID_TEXT];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(P.ids[i]), guid, sizeof guid);
            file_put(guid, NULL);
            for (int k = 0; k < g_nsaved; k++)
                if (!strcmp(g_saved[k], guid)) memcpy(g_saved[k], g_saved[--g_nsaved], GUID_TEXT);
            next_list = 0;
        }
    }
    if (P.begin) {
        int i = P.begin - 1;
        P.begin = 0;
        if (P.joy) stop();
        SDL_Joystick *j = i < P.count ? SDL_OpenJoystick(P.ids[i]) : NULL;
        if (j) {
            P.joy = j, P.active = true, P.step = PADMAP_REST, P.held = false;
            P.naxes = SDL_clamp(SDL_GetNumJoystickAxes(j), 0, MAX_AXES);
            P.nhats = SDL_clamp(SDL_GetNumJoystickHats(j), 0, MAX_HATS);
            P.nbuttons = SDL_clamp(SDL_GetNumJoystickButtons(j), 0, MAX_BUTTONS);
            memset(P.input, 0, sizeof P.input);
            LOG("[padmap] mapping %s: %d axes, %d hats, %d buttons\n", P.names[i], P.naxes, P.nhats, P.nbuttons);
        } else {
            P.active = false;
        }
    }
    if (P.joy && (P.cancel || !watched || !SDL_JoystickConnected(P.joy))) {
        LOG("[padmap] stopped: %s\n", P.cancel ? "cancelled" : !watched ? "the menu left" : "the controller is gone");
        stop();
    } else if (P.joy && P.save) {
        apply();
        stop();
        next_list = 0;
    }
    if (P.joy && P.step >= 0 && P.step < STEP_COUNT) {
        if (P.held) {
            if (all_released()) P.held = false;
        } else {
            char in[INPUT_TEXT];
            if (read_input(P.step, in)) {
                if (!used(in, P.step)) {
                    LOG("[padmap] %s: %s\n", STEPS[P.step].field, in);
                    memcpy(P.input[P.step++], in, INPUT_TEXT);
                }
                P.held = true;
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- the settings menu's
void padmap_watch(void) {
    pthread_mutex_lock(&g_mu);
    P.watched = plat_now_ns();
    pthread_mutex_unlock(&g_mu);
}

bool padmap_active(void) {
    pthread_mutex_lock(&g_mu);
    bool a = P.active;
    pthread_mutex_unlock(&g_mu);
    return a;
}

int padmap_list(char names[][64], bool *custom, int max) {
    pthread_mutex_lock(&g_mu);
    int n = P.count < max ? P.count : max;
    for (int i = 0; i < n; i++) {
        memcpy(names[i], P.names[i], 64);
        custom[i] = P.custom[i];
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

void padmap_begin(int index) {
    pthread_mutex_lock(&g_mu);
    P.begin = index + 1, P.watched = plat_now_ns();
    P.active = true, P.step = PADMAP_REST;  // (it opens on the main thread)
    pthread_mutex_unlock(&g_mu);
}

int padmap_step(void) {
    pthread_mutex_lock(&g_mu);
    int s = P.step;
    pthread_mutex_unlock(&g_mu);
    return s;
}

int padmap_steps(void) { return STEP_COUNT; }
const char *padmap_step_title(int step) { return step >= 0 && step < STEP_COUNT ? STEPS[step].title : ""; }

void padmap_continue(void) {
    pthread_mutex_lock(&g_mu);
    if (P.joy && P.step == PADMAP_REST) snapshot(), P.step = 0, P.held = false;
    pthread_mutex_unlock(&g_mu);
}

void padmap_skip(void) {
    pthread_mutex_lock(&g_mu);
    if (P.joy && P.step >= 0 && P.step < STEP_COUNT) P.input[P.step++][0] = 0, P.held = false;
    pthread_mutex_unlock(&g_mu);
}

void padmap_back(void) {
    pthread_mutex_lock(&g_mu);
    if (P.joy && P.step > 0) P.input[--P.step][0] = 0, P.held = true;
    pthread_mutex_unlock(&g_mu);
}

void padmap_save(void) {
    pthread_mutex_lock(&g_mu);
    P.save = true;
    pthread_mutex_unlock(&g_mu);
}

void padmap_cancel(void) {
    pthread_mutex_lock(&g_mu);
    P.cancel = true;
    pthread_mutex_unlock(&g_mu);
}

void padmap_forget(int index) {
    pthread_mutex_lock(&g_mu);
    P.forget = index + 1;
    pthread_mutex_unlock(&g_mu);
}
