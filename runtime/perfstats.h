// Performance readout ([Performance] ShowStats, runtime/perfstats.cpp): the frame rate, the frame time and, on a phone,
// its temperature, in a small plate at the top of the screen
#pragma once
#include <stdbool.h>

typedef union SDL_Event SDL_Event;

#ifdef __cplusplus
extern "C" {
#endif

void perfstats_poll(void);              // host main thread, each loop pass (runtime/ui.c): the phone's temperature, every 2 s at most
bool perfstats_visible(void);           // game thread (runtime/overlay.cpp): it is to be drawn this frame
void perfstats_draw(float w, float h);  // game thread, inside the overlay's ImGui frame (display size in pixels)
// [Performance] StatsX / StatsY: the plate dragged where it is wanted (runtime/overlay.cpp's events). Returns true when
// the event was the plate's (a press on it, its drag, its release), so the game and the menus do not also see it.
bool perfstats_drag_event(const SDL_Event *e);

#ifdef __cplusplus
}
#endif
