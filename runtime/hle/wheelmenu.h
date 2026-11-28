// The + menu's "Configure Shortcut Wheel" row (runtime/hle/wheelmenu.c).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
void wheelmenu_init(void);        // main thread, after the game's main module is mapped
bool wheelmenu_take_open(void);   // game frame: the row was confirmed once; open the wheel's settings page
#ifdef __cplusplus
}
#endif
