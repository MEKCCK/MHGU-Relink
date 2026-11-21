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
// Where the plate sits, for the touch layout editor (runtime/touch.cpp): the last drawn rectangle in screen pixels,
// and its place as a 0..1 centre of the screen.
bool perfstats_panel_rect(float *x0, float *y0, float *x1, float *y1);
void perfstats_set_pos(float cx, float cy, float w, float h);
void perfstats_save_pos(void);

#ifdef __cplusplus
}
#endif
