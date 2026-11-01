// The phone's vibration (Android: runtime/haptics.c -> com.mhgu.recomp.Haptics by JNI): a click for the touch
// controls' buttons and the game's rumble. Elsewhere these do nothing (a controller rumbles itself: host_input.c).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// host main thread (SDL's: the events, host_input_update and the lifecycle event watch)
void haptics_tap(void);           // a touch button pressed ([Touch] Vibration; at most one click per 25 ms)
void haptics_rumble(uint32_t v);  // every host input update: input_rumble() (low << 16 | high; [Controls] PhoneRumble)
void haptics_stop(void);          // stop at once (the app going to the background)

#ifdef __cplusplus
}
#endif
