// Mapping any controller: runtime/padmap.c
#pragma once
#include <stdbool.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif
#define PADMAP_MAX 8
// the SDL main thread (runtime/host_input.c)
void padmap_init(void);    // the mappings kept from before
void padmap_poll(void);    // each input update
bool padmap_custom(SDL_JoystickID id);  // this controller was mapped by the player (a mapping says by position)
// any thread: being mapped now, so its buttons are neither the game's nor the menu's
bool padmap_active(void);
// the settings menu's. Each frame on its pages it says padmap_watch(): the list is kept fresh, and leaving the pages
// by any way (Back, closing the menu) ends the mapping
void padmap_watch(void);
int padmap_list(char names[][64], bool *custom, int max);
void padmap_begin(int index);
int padmap_step(void);  // PADMAP_REST first, then 0 .. padmap_steps() - 1, then padmap_steps(): done
#define PADMAP_REST (-1)
int padmap_steps(void);
const char *padmap_step_title(int step);  // English: settings_text translates it
void padmap_continue(void);  // rest: everything was moved once and let go
void padmap_skip(void);
void padmap_back(void);
void padmap_save(void);
void padmap_cancel(void);
void padmap_forget(int index);  // back to what SDL makes of it, the next time the game starts
#ifdef __cplusplus
}
#endif
