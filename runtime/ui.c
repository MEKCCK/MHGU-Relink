// The game window and the host main thread's event loop (the guest runs on its own threads). The window is created
// before the guest starts so the renderer can attach to it from the game's graphics thread.
//   MHGU_HIDDEN=1     no visible window (test runs; controls then come only from MHGU_INPUT)
// Display mode, size and the 3D resolution come from the settings (runtime/config.c, mhgurecomp.ini).
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "config.h"
#include "app_actions.h"
#include "haptics.h"
#include "overlay.h"
#include "mods.h"
#include "perfstats.h"
#include "rt.h"
#include "touch.h"
#include "voice.h"
#include "hle/input.h"

static int launch_argc;
static char **launch_argv;
static SDL_AtomicInt requested_action, action_failed;
void ui_capture_launch_args(int argc, char **argv) { launch_argc = argc; launch_argv = argv; }
void ui_request_action(bool restart) { SDL_SetAtomicInt(&requested_action, restart ? 2 : 1); }
// (B on the title screen's Main Menu. 12 is Android's files folder, so it has a number of its own)
void ui_request_exit_menu(void) { SDL_SetAtomicInt(&requested_action, 14); }
#ifndef __ANDROID__
void ui_request_import(int what) { SDL_SetAtomicInt(&requested_action, 2 + what); }
#endif
bool ui_action_failed(void) { return SDL_SetAtomicInt(&action_failed, 0) != 0; }
#ifndef __ANDROID__
// PC: the system's folder picker (SDL_ShowOpenFolderDialog), for request 4. SDL may call back on another thread.
static char picked_path[1100];
static SDL_AtomicInt picked_ready;
static void folder_picked(void *what, const char *const *list, int filter) {
    (void)filter;
    if (!list) { LOG("[ui] folder picker: %s\n", SDL_GetError()); return; }
    if (!list[0]) return;  // (cancelled)
    SDL_strlcpy(picked_path, list[0], sizeof picked_path);
    SDL_SetAtomicInt(&picked_ready, (int)(intptr_t)what);
}
int ui_take_picked_folder(char *out, size_t size) {
    int what = SDL_SetAtomicInt(&picked_ready, 0);
    if (what) SDL_strlcpy(out, picked_path, size);
    return what;
}
#endif
#ifdef __ANDROID__
void ui_request_licenses(void) { SDL_SetAtomicInt(&requested_action, 3); }
void ui_request_changelog(void) { SDL_SetAtomicInt(&requested_action, 15); }
void ui_request_game_folder(void) { SDL_SetAtomicInt(&requested_action, 4); }
void ui_request_save_import(void) { SDL_SetAtomicInt(&requested_action, 5); }
void ui_request_share_log(void) { SDL_SetAtomicInt(&requested_action, 6); }
void ui_request_save_export(void) { SDL_SetAtomicInt(&requested_action, 7); }
void ui_request_gpu_driver(void) { SDL_SetAtomicInt(&requested_action, 8); }
void ui_request_import(int kind) { SDL_SetAtomicInt(&requested_action, 9 + kind); }  // (0 cheats, 1 a mod, 2 DLC, 3 the files folder, 4 the app's folder again)
static void activity_call_language(const char *method) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, method, "(I)V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m, (jint)config_language());
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif
#ifdef __ANDROID__
static void frame_cap_to_activity(float fps) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "setFrameCap", "(F)V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m, (jfloat)fps);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
// The phone's gyro is registered only while [Controls] Motion has it (runtime/host_input.c turns its samples into the
// hunt camera's turns); MotionSensor.java unregisters it on its own when the app goes to the background.
static void motion_to_activity(bool on) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "setMotionEnabled", "(Z)V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m, on ? JNI_TRUE : JNI_FALSE);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif
static bool restart_application(void) {
#ifdef __ANDROID__
    // No process of our own here: GameActivity.restartApp() starts PhoenixActivity (process ":phoenix"), which waits
    // for this one to leave (the caller's exit(0): the atexit pipeline-cache save) and launches the app again. (The
    // activity's own class, not FindClass: the app's classes are not in a native thread's class loader.)
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) { LOG("[ui] restart failed: no activity (%s)\n", SDL_GetError()); return false; }
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "restartApp", "()V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m);
    bool ok = m && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    LOG(ok ? "[ui] restart: PhoenixActivity relaunches the app once this process has left\n"
           : "[ui] restart failed: GameActivity.restartApp() (see logcat)\n");
    return ok;
#else
    char exe[4096], wait[80];
    if (!plat_exe_path(exe, sizeof exe)) return false;
    const char **args = calloc((size_t)launch_argc + 3, sizeof *args);
    if (!args) return false;
    int n = 0;
    args[n++] = exe;
    for (int i = 1; i < launch_argc; ++i)
        if (strncmp(launch_argv[i], "--recomp-wait=", 14)) args[n++] = launch_argv[i];
    snprintf(wait, sizeof wait, "--recomp-wait=%lu", plat_process_id());
    args[n++] = wait;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_Process *child = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    free(args);
    if (!child) { LOG("[ui] restart failed: %s\n", SDL_GetError()); return false; }
    LOG("[ui] restart child %lld waiting for %lu\n", (long long)SDL_GetNumberProperty(
        SDL_GetProcessProperties(child), SDL_PROP_PROCESS_PID_NUMBER, 0), plat_process_id());
    SDL_DestroyProcess(child);
    return true;
#endif
}

volatile uint64_t g_present_count;
static SDL_Window *window;

SDL_Window *ui_window(void) { return window; }

// The window's size when windowed: the settings' size (or the last windowed size), no larger than the desktop's free
// area with the window frame.
static int win_w, win_h;
static void fit_desktop(int *w, int *h) {
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r)) return;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (window) SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
    if (!top) top = 32, left = right = bottom = 8;  // (not known before the window exists: typical Windows 10 frame)
    r.w -= left + right, r.h -= top + bottom;
    if (*w <= r.w && *h <= r.h) return;
    int fw = r.w, fh = r.w * *h / *w;
    if (fh > r.h) fh = r.h, fw = r.h * *w / *h;
    LOG("[ui] %dx%d does not fit the desktop: window %dx%d\n", *w, *h, fw, fh);
    *w = fw, *h = fh;
}

// Full screen as the settings say (exclusive: the monitor mode closest to Resolution; else borderless desktop), or
// windowed. The cursor only shows in a window.
static void set_fullscreen(bool on) {
    bool was = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
    if (on && !was) SDL_GetWindowSize(window, &win_w, &win_h);
    if (on && g_config.display_mode == DISPLAY_FULLSCREEN) {
        SDL_DisplayMode m;
        SDL_DisplayID d = SDL_GetDisplayForWindow(window);
        if (SDL_GetClosestFullscreenDisplayMode(d, g_config.width, g_config.height, 0.0f, false, &m)) {
            SDL_SetWindowFullscreenMode(window, &m);
            LOG("[ui] fullscreen %dx%d @ %.0f Hz\n", m.w, m.h, m.refresh_rate);
        } else {
            SDL_SetWindowFullscreenMode(window, NULL);
            LOG("[ui] no fullscreen mode near %dx%d (%s): borderless instead\n", g_config.width, g_config.height,
                SDL_GetError());
        }
    } else if (on) {
        SDL_SetWindowFullscreenMode(window, NULL);
    }
    SDL_SetWindowFullscreen(window, on);
    if (!on && was) {  // (SDL restores the frame's size as the client size after an exclusive mode: set it again)
        SDL_SyncWindow(window);
        fit_desktop(&win_w, &win_h);
        SDL_SetWindowSize(window, win_w, win_h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (on) SDL_HideCursor(); else SDL_ShowCursor();
}

#ifdef __ANDROID__
// The activity's lifecycle, as it happens (an event watch: these must be acted on before the call returns): the
// renderer draws nothing while the window is gone and makes its surface again after (nvn/vulkan/device.c); with
// [Application] PauseInBackground the game stops meanwhile too (runtime/hle/vi.c).
extern volatile int g_android_window_state;
void vi_set_paused(bool on);
extern bool ldn_net_up(void);
static bool android_lifecycle(void *ud, SDL_Event *e) {
    (void)ud;
    switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        __atomic_store_n(&g_android_window_state, 1, __ATOMIC_RELEASE);
        // (SDL's main thread now blocks until the app is back: the buttons and sticks it gave the game would stay held,
        // and the fingers' ups will not come)
        input_set(0, 0, 0, 0, 0);
        touch_reset();
        haptics_stop();  // (a rumble playing would run out its last pulse)
        if (config_live_pause_background() && !ldn_net_up()) vi_set_paused(true);
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        __atomic_store_n(&g_android_window_state, 2, __ATOMIC_RELEASE);
        vi_set_paused(false);  // (after the state: the renderer's first frame back makes the surface again)
        break;
    default: break;
    }
    return true;
}
#endif

// The window's icon. Windows takes it from the exe (extras/icon/mhgu-relink.rc) and Android from the launcher icon, so only
// Linux sets one: extras/icon/mhgu-relink-128.png, built into the program (the path is relative to the repo, where make runs).
#if !defined(_WIN32) && !defined(__ANDROID__)
__asm__(".pushsection .rodata\n.globl mhgu_icon_png\nmhgu_icon_png:\n.incbin \"extras/icon/mhgu-relink-128.png\"\n"
        ".globl mhgu_icon_png_end\nmhgu_icon_png_end:\n.popsection");
extern const unsigned char mhgu_icon_png[], mhgu_icon_png_end[];
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform.h"

// A Linux executable can't carry an icon for the file manager, so the desktop finds the game through a launcher:
// $XDG_DATA_HOME/applications/mhgu-relink.desktop (~/.local/share/applications) and the icon beside it in
// icons/hicolor/128x128/apps/mhgu-relink.png, and a copy as "MHGU Relink.desktop" in the game's folder. Written at each
// start when missing or out of date (the folder moved), so the app menu starts this copy: mhgu-relink-updater when it is
// beside the game (it starts the game, and with Updates = Off only that), else the game. A launcher without the
// X-MHGURecomp line is the player's own and left alone. The windows' app id is the launcher's name (SDL: the
// executable's name; the updater sets it), so the taskbar shows the icon too.
static bool write_if_changed(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "rb");
    if (f) {
        char old[8192];
        size_t n = fread(old, 1, sizeof old, f);
        fclose(f);
        if (n == size && !memcmp(old, data, size)) return true;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

static void mkdirs(char *path) {  // (each folder of path, which is a folder)
    for (char *p = path + 1; *p; ++p)
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    mkdir(path, 0755);
}

static void install_launcher(void) {
    if (getenv("MHGU_HIDDEN") || getenv("MHGU_NO_LAUNCHER")) return;
    char exe[PATH_MAX], data[PATH_MAX], dir[PATH_MAX + 64], path[PATH_MAX + 96];
    if (!plat_exe_path(exe, sizeof exe)) return;
    if (strpbrk(exe, "\"`$\\%\n\r")) { LOG("[ui] no app menu launcher: the game's path has a \" ` $ \\ or %% in it\n"); return; }
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') snprintf(data, sizeof data, "%s", xdg);
    else if (home && *home == '/') snprintf(data, sizeof data, "%s/.local/share", home);
    else return;
    char *slash = strrchr(exe, '/');
    char run[PATH_MAX + 32];
    snprintf(run, sizeof run, "%.*s/mhgu-relink-updater", (int)(slash - exe), exe);
    if (access(run, X_OK) != 0) snprintf(run, sizeof run, "%s", exe);

    snprintf(path, sizeof path, "%s/applications/mhgu-relink.desktop", data);
    FILE *f = fopen(path, "rb");
    if (f) {
        char old[8192];
        size_t n = fread(old, 1, sizeof old - 1, f);
        fclose(f);
        old[n] = 0;
        if (!strstr(old, "\nX-MHGURecomp=true\n")) return;
    }
    snprintf(dir, sizeof dir, "%s/icons/hicolor/128x128/apps", data);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/mhgu-relink.png", dir);
    if (!write_if_changed(path, mhgu_icon_png, (size_t)(mhgu_icon_png_end - mhgu_icon_png))) {
        LOG("[ui] no app menu launcher: can't write %s\n", path);
        return;
    }
    // (Icon: the file's full path, which every desktop finds; a theme name only works where the icon cache has caught up,
    // and a copy dragged to Plasma's desktop showed a "?")
    char entry[4 * PATH_MAX + 512];
    int n = snprintf(entry, sizeof entry,
                     "[Desktop Entry]\nType=Application\nName=MHGU Relink\nComment=Monster Hunter Generations Ultimate, "
                     "recompiled\nExec=\"%s\"\nPath=%.*s\nIcon=%s\nTerminal=false\nCategories=Game;\n"
                     "StartupWMClass=mhgu-relink\nX-MHGURecomp=true\n",
                     run, (int)(slash - exe), exe, path);
    if (n <= 0 || (size_t)n >= sizeof entry) return;
    snprintf(dir, sizeof dir, "%s/applications", data);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/mhgu-relink.desktop", dir);
    if (!write_if_changed(path, entry, (size_t)n)) LOG("[ui] no app menu launcher: can't write %s\n", path);
    // The same launcher beside the game, so the folder has something to click with the icon (file managers start an
    // executable .desktop file; a read-only folder just goes without)
    snprintf(path, sizeof path, "%.*s/MHGU Relink.desktop", (int)(slash - exe), exe);
    if (write_if_changed(path, entry, (size_t)n)) chmod(path, 0755);
}

static void set_window_icon(void) {
    install_launcher();
    SDL_Surface *s = SDL_LoadPNG_IO(SDL_IOFromConstMem(mhgu_icon_png, (size_t)(mhgu_icon_png_end - mhgu_icon_png)), true);
    if (!s) { LOG("[ui] window icon: %s\n", SDL_GetError()); return; }
    SDL_SetWindowIcon(window, s);
    SDL_DestroySurface(s);
}
#else
static void set_window_icon(void) {}
#endif

void ui_init(void) {
#ifdef __ANDROID__
    // Touches are not mouse clicks here (a click is X / A and the mouse turns the camera: host_input.c), and the
    // window is the screen, in landscape.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int sdl = SDL_GetVersion();
#ifdef MHGU_SDL_BUNDLED
    extern char g_sdl_choice[];  // (runtime/platform/linux_sdl.c)
    LOG("[ui] SDL %d.%d.%d, %s; video %s\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
        SDL_VERSIONNUM_MICRO(sdl), g_sdl_choice, SDL_GetCurrentVideoDriver());
#else
    LOG("[ui] SDL %d.%d.%d; video %s\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl),
        SDL_GetCurrentVideoDriver());
#endif
    config_load();
#ifdef __ANDROID__
    if (g_config.save_dir[0]) g_save_dir = g_config.save_dir;  // ([Android] SaveFolder: MHGU_SAVE_DIR still wins, fs.c)
#else
    if (g_config.save_location[0]) {  // ([Application] SaveLocation: MHGU_SAVE_DIR still wins, fs.c)
        static char save[1100];
        if (plat_use_folder(g_config.save_location, save, sizeof save)) {
            g_save_dir = save;
            LOG("[ui] save folder (SaveLocation): %s\n", save);
        } else
            LOG("[ui] SaveLocation \"%s\" can't be used (made or opened): the save stays in %s\n", g_config.save_location,
                g_save_dir);
    }
#endif
    // Initial background input policy. SDL documents this hint as changeable at runtime.
    // Subsequent saved changes are consumed by the host thread in ui_run().
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_config.background_input ? "1" : "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) FATAL("SDL_InitSubSystem: %s\n", SDL_GetError());
    win_w = g_config.width, win_h = g_config.height;
#ifndef __ANDROID__  // (there the window is the screen)
    fit_desktop(&win_w, &win_h);
#endif
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifndef MHGU_NULL_BACKEND
    flags |= SDL_WINDOW_VULKAN;
#endif
    if (getenv("MHGU_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Monster Hunter Generations Ultimate", win_w, win_h, flags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    if (g_config.display_mode != DISPLAY_WINDOWED) set_fullscreen(true);
#ifdef __ANDROID__
    SDL_AddEventWatch(android_lifecycle, NULL);
#else
    set_window_icon();
#endif
}

// Alt+Enter: windowed <-> the full-screen mode of the settings (Windowed in the settings: borderless).
void ui_toggle_fullscreen(void) {
    set_fullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}

// The window is going away (Android: the activity is in the background): nothing is drawn meanwhile.
bool ui_window_away(void) {
#ifdef __ANDROID__
    extern volatile int g_android_window_state;  // (nvn/vulkan/device.c: 1 in the background, 2 back)
    return __atomic_load_n(&g_android_window_state, __ATOMIC_ACQUIRE) == 1;
#else
    return false;
#endif
}

int ui_run(void) {
    int background = config_live_background_input();
    for (;;) {
        int wanted_background = config_live_background_input();
        if (wanted_background != background) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, wanted_background ? "1" : "0");
            background = wanted_background;
        }
        SDL_Event e;
#ifdef __ANDROID__
        // (SDL's WaitEventTimeout spins on Android - a whole core in its joystick polling and timed mutex waits, measured
        // on a Galaxy S25: the events are taken as they are, then a 3 ms sleep)
        bool got = SDL_PollEvent(&e);
        if (!got) SDL_Delay(3), got = SDL_PollEvent(&e);
        if (got) {
#else
        if (SDL_WaitEventTimeout(&e, 4)) {
#endif
            do {
                if (e.type == SDL_EVENT_QUIT) {
                    if (!config_confirm_exit() && !overlay_settings_dirty()) { LOG("window closed\n"); exit(0); }
                    overlay_request_close();
                    continue;
                }
                host_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        int action = SDL_SetAtomicInt(&requested_action, 0);
        if (action == 14) {  // (the title screen's B: the same question as closing the window)
            if (!config_confirm_exit() && !overlay_settings_dirty()) {
                LOG("exit from the title screen\n");
                exit(0);
            }
            overlay_request_close();
        }
        if (action == 1) exit(0);
#ifdef __ANDROID__
        if (action == 3) activity_call_language("showLicenses");
        if (action == 15) activity_call_language("showChangelog");
        if (action == 4) activity_call_language("showGameFolder");
        if (action == 5) activity_call_language("showSaveImport");
        if (action == 6) activity_call_language("showShareLog");
        if (action == 7) activity_call_language("showSaveExport");
        if (action == 8) activity_call_language("showDrivers");
        if (action == 9) activity_call_language("importCheats");
        if (action == 10) activity_call_language("importMod");
        if (action == 11) activity_call_language("importDlc");
        if (action == 12) activity_call_language("chooseFilesFolder");
        if (action == 13) activity_call_language("useAppFilesFolder");
        static float screen_for = -1;  // (the screen's refresh rate follows the FPS limit: GameActivity.setFrameCap)
        float fps_now = config_live_fps();
        if (fps_now != screen_for) screen_for = fps_now, frame_cap_to_activity(fps_now);
        static int motion_for = -1;  // ([Controls] Motion: the gyro is read only while a mode has it)
        int motion_now = host_input_motion_phone_requested();
        if (motion_now != motion_for) motion_for = motion_now, motion_to_activity(motion_now);
#endif
#ifndef __ANDROID__
        if (action == 6)  // (request 4: the save's folder. SDL wants its dialogs started on this thread)
            SDL_ShowOpenFolderDialog(folder_picked, (void *)(intptr_t)4, window,
                                     g_config.save_location[0] ? g_config.save_location : NULL, false);
#endif
        if (action == 2) {
            if (restart_application()) exit(0);
            SDL_SetAtomicInt(&action_failed, 1);
        }
        host_input_update();
        voice_tick();  // (voice chat: opened or closed with the network and the mode, the microphone and the speakers)
        cheats_frame();
        perfstats_poll();  // ([Performance] ShowStats: the phone's temperature, every 2 s at most)
    }
}
