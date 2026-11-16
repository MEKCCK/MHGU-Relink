// Touch controls (phones): a floating stick on the left, the camera turned by dragging anywhere else, and the game's
// buttons in the Monster Hunter style around the right thumb (runtime/touch.cpp)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// host main thread (runtime/host_input.c)
// Map an SDL normalized finger position through the same renderer menu transform as the mouse.
bool host_input_menu_point(float u, float v, float *x, float *y);
bool touch_event(const SDL_Event *e);  // a finger event: true when it was the controls'
// ORs into buttons and returns the buttons contributed by touch, including overlaps with other input sources.
uint64_t touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, int *rx, int *ry,
                     float *cam_dx, float *cam_dy);
bool touch_running(void);  // full left stick requests running only, independently of native R
bool touch_virtual_right_stick_active(void);  // the virtual stick remains native input; its axes are not camera input
// game thread (runtime/overlay.cpp): drawn over the game, under the port's menus
bool touch_visible(void);
bool touch_drawn(void);  // (something of them is on the screen: the settings button alone, when they are hidden)
void touch_draw(float display_w, float display_h);
void touch_edit_begin(void);  // the layout editor (settings menu: Controls), once the menu is closed
void touch_edit_cancel(void);  // (the settings menu's unsaved-changes dialog cancelled: it stays open)
enum { TOUCH_TEMPLATE_COUNT = 40, TOUCH_TEMPLATE_NAME_MAX = 64 };
// Templates store positions, sizes, HUD offsets, icons and all six combo definitions and names.
bool touch_template_info(int slot, char *name, int name_size); // false for empty or invalid slots (0..39)
bool touch_template_save(int slot, const char *name); // snapshot the current layout, including an editor draft
bool touch_template_use(int slot); // save and apply the snapshot; unavailable during layout editing
enum { TOUCH_ICON_BUTTON_COUNT = 25, TOUCH_ICON_HASH_SIZE = 65 };
// Icon choice: 0 default, 1..builtin_count built-in, -1 immutable custom image.
const char *touch_icon_button_name(int button);
int touch_icon_builtin_count(void);
const char *touch_icon_builtin_name(int choice);
bool touch_icon_get(int button, int *choice, char hash[TOUCH_ICON_HASH_SIZE]);
uint64_t touch_layout_generation(void);
// Reject stale picker results; persist only after a complete validated image and successful layout save.
bool touch_icon_set(int button, int choice, const char *hash, uint64_t expected_generation);
void touch_lock_toggle(void);  // the lock-on (a controller's right stick click in a hunt: runtime/host_input.c)
// the lock-on's L presses, given every button pressed now and whether the camera is being turned (host_input.c)
uint64_t touch_lock_buttons(uint64_t buttons, bool cam_input);
void touch_reset(void);  // every finger gone (the app went to the background: runtime/ui.c)
void touch_background(void);  // also retain whether an in-game menu was open for foreground touch routing
// while shown, the HUD moves out of the thumbs' way: the status to the left edge, the item bar to the bottom middle,
// the Hunter Arts and the target camera to the top right (nvn/vulkan/exec.c)
extern volatile int g_touch_hud;
// Where those moved HUD parts are drawn, so a finger on them works them: a tap on the item bar uses the item and a
// swipe turns it, a tap on a Hunter Art's panel performs it, a tap on the target camera's icon enables the lock-on
// and subsequent taps select the next monster; holding the icon for 500 ms toggles the lock. The render thread passes
// each moved part's place (the game's
// position, and where it went on the 1280x720 canvas: nvn/vulkan/exec.c cbuf_info), and closes each frame (device.c).
enum { TZ_ITEM, TZ_ARTS, TZ_TARGET, TZ_MAP, TZ_COUNT };
// where the player put them in the layout editor: an offset from their default place, in the game's 16:9 canvas
// units (1280 x 720); the render thread adds it (nvn/vulkan/exec.c)
extern volatile float g_touch_zone_dx[TZ_COUNT], g_touch_zone_dy[TZ_COUNT];
// offset_x/y are the scaled manual displacement actually applied to this piece, excluding automatic MoveHUD.
void touch_hud_piece(int zone, float game_x, float game_y, float canvas_x, float canvas_y,
                     float offset_x, float offset_y);
// Optional native Hunter Art direction icon: slot 0 / 1 / 2 is Up / Right / Down; row taps work without it.
// The render thread may report it before or after the row pieces; touch_hud_frame publishes them together.
void touch_hud_art_slot(float game_y, int slot);
void touch_hud_frame(void);
void touch_hud_menu(void);  // (render thread) a menu window of the game's was drawn this frame: the menu layout

#ifdef __cplusplus
}
#endif
