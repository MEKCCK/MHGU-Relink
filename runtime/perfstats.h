// Performance readout ([Performance] ShowStats, runtime/perfstats.cpp): the frame rate, the frame time and, on a phone,
// its temperature, in a small plate at the top of the screen
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void perfstats_poll(void);              // host main thread, each loop pass (runtime/ui.c): the phone's temperature, every 2 s at most
bool perfstats_visible(void);           // game thread (runtime/overlay.cpp): it is to be drawn this frame
void perfstats_draw(float w, float h);  // game thread, inside the overlay's ImGui frame (display size in pixels)

#ifdef __cplusplus
}
#endif
