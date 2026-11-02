// The Vulkan driver: the system's, as always, unless the player picked one of their own (Settings, Graphics, GPU driver:
// android/.../DriverActivity.java), for an Adreno, at their own risk.
//
// The renderer's vk* are function pointers (third_party/volk, force-included by native/CMakeLists.txt), filled from the
// vkGetInstanceProcAddr given here. With the system's driver that is the one of the system's libvulkan.so: every call
// reaches the same functions the library exports. A driver of one's own takes libadrenotools: a private copy of
// libvulkan.so, hooked to load that driver instead of the system's.
//
// A driver that doesn't work must not leave the game unable to start. A trial file is written before it is opened and
// removed once frames have been drawn with it (gpu_driver_good): found at the next start, that run never got there, so the
// choice is moved to failed (DriverActivity tells the player) and the system's driver is used.
//
// files/gpu_drivers/: <folder>/<driver>.so, selected (its folder, then its library's file name), failed, trial
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <android/native_window.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "../rt.h"
#include "adrenotools/driver.h"

static bool g_custom;
static char g_name[128];

bool gpu_driver_custom(void) { return g_custom; }
const char *gpu_driver_name(void) { return g_name; }

static void chomp(char *s) { s[strcspn(s, "\r\n")] = 0; }

PFN_vkGetInstanceProcAddr gpu_driver_loader(void) {
    void *lib = NULL;
    char root[400], selected[512], trial[512], failed[512];
    const char *home = SDL_GetAndroidInternalStoragePath();
    snprintf(root, sizeof root, "%s/gpu_drivers", home ? home : ".");
    snprintf(selected, sizeof selected, "%s/selected", root);
    snprintf(trial, sizeof trial, "%s/trial", root);
    snprintf(failed, sizeof failed, "%s/failed", root);
    FILE *f = home ? fopen(selected, "r") : NULL;
    if (f) {
        char folder[128] = "", file[128] = "";
        if (!fgets(folder, sizeof folder, f)) folder[0] = 0;
        if (!fgets(file, sizeof file, f)) file[0] = 0;
        fclose(f);
        chomp(folder), chomp(file);
        if (access(trial, F_OK) == 0) {  // (the last run with it drew nothing: back to the system's)
            LOG("[gpu] driver %s failed in the last run: the system's instead\n", folder);
            rename(selected, failed);
            unlink(trial);
        } else if (folder[0] && file[0]) {
            char dir[560], hooks[512] = "";
            snprintf(dir, sizeof dir, "%s/%s/", root, folder);
            Dl_info me;  // (the hooks are beside this library: the app's nativeLibraryDir)
            if (dladdr((void *)gpu_driver_loader, &me) && me.dli_fname) {
                snprintf(hooks, sizeof hooks, "%s", me.dli_fname);
                char *slash = strrchr(hooks, '/');
                if (slash) slash[1] = 0;
            }
            FILE *t = fopen(trial, "w");
            if (t) fclose(t);
            lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, file, NULL, NULL);
            if (lib) {
                g_custom = true;
                snprintf(g_name, sizeof g_name, "%s", folder);
                LOG("[gpu] driver of the player's: %s (%s)\n", folder, file);
            } else {
                LOG("[gpu] driver %s could not be opened: %s; the system's instead\n", folder, dlerror());
                rename(selected, failed);
                unlink(trial);
            }
        }
    }
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
}

// Frames have been drawn with it
void gpu_driver_good(void) {
    if (!g_custom) return;
    char trial[512];
    snprintf(trial, sizeof trial, "%s/gpu_drivers/trial", SDL_GetAndroidInternalStoragePath());
    unlink(trial);
    LOG("[gpu] driver %s works\n", g_name);
}

// A surface on the same libvulkan as the instance: SDL's own is the system's
bool gpu_driver_surface(SDL_Window *win, VkInstance inst, VkSurfaceKHR *out) {
    if (!g_custom) return SDL_Vulkan_CreateSurface(win, inst, NULL, out);
    struct ANativeWindow *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    if (!nw) return SDL_SetError("no Android window");
    VkAndroidSurfaceCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = nw};
    VkResult r = vkCreateAndroidSurfaceKHR(inst, &ci, NULL, out);
    return r == VK_SUCCESS ? true : SDL_SetError("vkCreateAndroidSurfaceKHR: %d", r);
}
