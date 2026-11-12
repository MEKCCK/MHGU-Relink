#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_capture_launch_args(int argc, char **argv);
#ifdef __ANDROID__
void ui_request_licenses(void);     // The licenses' and the game folder's screens, opened on the host thread too
void ui_request_game_folder(void);
void ui_request_save_import(void);
void ui_request_save_export(void);
void ui_request_gpu_driver(void);
void ui_request_import(int kind);  // the import screen (ImportActivity): 0 cheats, 1 a mod, 2 DLC
bool vk_gpu_adreno(void);          // (nvn/vulkan/device.c)
bool vk_gpu_adreno_fix_applies(void);  // (an Adreno on a Vulkan 1.1 driver: [Performance] AdrenoFix)
const char *gpu_driver_name(void);  // (platform/android_gpu.c: "" the system's)
void ui_request_share_log(void);
#endif
void ui_request_action(bool restart); // Called after the in-game confirmation, handled on the host thread.
void ui_request_exit_menu(void);      // Title screen's Main Menu, B: the window-close question (host thread).
bool ui_action_failed(void);          // Take and clear a failed restart notification.
#ifndef __ANDROID__
// PC: 1 the game files, 2 a save in, 3 the save out, 4 the save's folder, 5 a mod from a zip, 6 a mod from a
// folder, 7 the game files' folder. (Android keeps its own ui_request_import in the block above: 0 cheats,
// 1 a mod, 2 DLC.)
void ui_request_import(int what);
// The folder chosen by request 4, once (0: none). The system's folder picker (SDL_ShowOpenFolderDialog).
int ui_take_picked_folder(char *out, size_t size);
#endif
#ifdef __cplusplus
}
#endif
