// Keys the keyboard controls already use (runtime/host_input.c key_buttons and its other keys, runtime/overlay.cpp):
// the settings menu marks a chat key on one of them (settings_panel.cpp). Keep in step with those.
#pragma once
#include <SDL3/SDL.h>

static inline const char *key_in_use(int scancode) {
    static const struct { int scancode; const char *use; } keys[] = {
        {SDL_SCANCODE_W, "Move"}, {SDL_SCANCODE_A, "Move"}, {SDL_SCANCODE_S, "Move"}, {SDL_SCANCODE_D, "Move"},
        {SDL_SCANCODE_LSHIFT, "Walk"}, {SDL_SCANCODE_SPACE, "B"}, {SDL_SCANCODE_R, "Y"}, {SDL_SCANCODE_Q, "L"},
        {SDL_SCANCODE_E, "R"}, {SDL_SCANCODE_TAB, "ZL"}, {SDL_SCANCODE_T, "ZR"}, {SDL_SCANCODE_RETURN, "Plus"},
        {SDL_SCANCODE_KP_ENTER, "Plus"}, {SDL_SCANCODE_BACKSPACE, "Minus"}, {SDL_SCANCODE_UP, "D-pad"},
        {SDL_SCANCODE_DOWN, "D-pad"}, {SDL_SCANCODE_LEFT, "D-pad"}, {SDL_SCANCODE_RIGHT, "D-pad"},
        {SDL_SCANCODE_LCTRL, "Left stick click"}, {SDL_SCANCODE_RCTRL, "Left stick click"},
        {SDL_SCANCODE_LALT, "Right stick click"}, {SDL_SCANCODE_RALT, "Right stick click"},
        {SDL_SCANCODE_ESCAPE, "Release mouse"}, {SDL_SCANCODE_F1, "Settings"}, {SDL_SCANCODE_INSERT, "Forge menu"},
        {SDL_SCANCODE_F7, "Debug"}, {SDL_SCANCODE_F8, "Debug"}, {SDL_SCANCODE_F9, "Debug"}, {SDL_SCANCODE_F10, "Debug"},
        {SDL_SCANCODE_F11, "Full screen"}, {SDL_SCANCODE_F12, "Screenshot"},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (keys[i].scancode == scancode) return keys[i].use;
    return NULL;
}
