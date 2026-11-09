// Exercise the real Android text-input state machine with a fake SDL keyboard.
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool menu_open, input_active, keyboard_shown, keyboard_enabled = true;
static int starts, stops;
static SDL_Window *window = (SDL_Window *)1;
static bool mock_hint(const char *name, const char *value) {
    assert(!strcmp(name, SDL_HINT_ENABLE_SCREEN_KEYBOARD));
    keyboard_enabled = !strcmp(value, "1");
    return true;
}
static bool mock_start(SDL_Window *w) {
    assert(w == window);
    starts++; input_active = true; keyboard_shown = keyboard_enabled;
    return true;
}
static bool mock_stop(SDL_Window *w) {
    assert(w == window);
    stops++; input_active = keyboard_shown = false;
    return true;
}
static bool mock_active(SDL_Window *w) { assert(w == window); return input_active; }
static bool mock_shown(SDL_Window *w) { assert(w == window); return keyboard_shown; }
#define SDL_SetHint mock_hint
#define SDL_StartTextInput mock_start
#define SDL_StopTextInput mock_stop
#define SDL_TextInputActive mock_active
#define SDL_ScreenKeyboardShown mock_shown
#define pthread_mutex_t int
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(x) ((void)(x))
#define pthread_mutex_unlock(x) ((void)(x))
#define __ANDROID__ 1
#include "../runtime/textinput.c"

SDL_Window *ui_window(void) { return window; }
bool overlay_menu_open(void) { return menu_open; }

int main(void) {
    menu_open = true; textinput_update();
    assert(g_window_on == 2 && input_active && !keyboard_shown);
    textinput_menu_keyboard(true); textinput_update();
    assert(g_window_on == 3 && keyboard_shown && keyboard_enabled);
    int before = starts; textinput_update(); assert(starts == before);
    // Android Back hides the IME: keep physical typing active without reopening it.
    input_active = keyboard_shown = false; textinput_update();
    assert(input_active && !keyboard_shown && keyboard_enabled);
    before = starts; textinput_update(); assert(starts == before);
    textinput_menu_reopen(); textinput_update(); assert(keyboard_shown);
    textinput_menu_keyboard(false); textinput_update();
    assert(g_window_on == 2 && input_active && !keyboard_shown);
    // Returning from a confirmation to the editor opens it again; closing the menu hides it.
    textinput_menu_keyboard(true); textinput_update(); assert(keyboard_shown);
    menu_open = false; textinput_update();
    assert(g_window_on == 0 && !input_active && !keyboard_shown);
    textinput_menu_keyboard(false);
    // Game text entries retain their existing UTF-8 input and keyboard behavior.
    textinput_begin("", 20); textinput_update();
    assert(g_window_on == 1 && keyboard_shown);
    SDL_Event e = {0}; e.type = SDL_EVENT_TEXT_INPUT; e.text.text = "room.123";
    assert(textinput_event(&e));
    char value[64]; int chars = 0; assert(textinput_poll(value, sizeof value, &chars) == TI_CHANGED);
    assert(!strcmp(value, "room.123") && chars == 8);
    e.text.text = "\xE4\xB8\xAD\xE6\x96\x87";
    assert(textinput_event(&e));
    assert(textinput_poll(value, sizeof value, &chars) == TI_CHANGED);
    assert(!strcmp(value, "room.123\xE4\xB8\xAD\xE6\x96\x87") && chars == 10);
    e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_BACKSPACE;
    assert(textinput_event(&e));
    assert(textinput_poll(value, sizeof value, &chars) == TI_CHANGED);
    assert(!strcmp(value, "room.123\xE4\xB8\xAD") && chars == 9);
    e.key.scancode = SDL_SCANCODE_RETURN;
    assert(textinput_event(&e));
    assert(textinput_poll(value, sizeof value, &chars) == TI_ENTER);
    textinput_end(); textinput_update(); assert(!input_active && !keyboard_shown);
    assert(stops > 0);
    puts("Android settings IME state transitions passed");
    return 0;
}
