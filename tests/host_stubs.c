// Host-only tests link the config/cheats code without runtime/main.c: the two globals those files read.
// Weak: a test that defines one itself (tests/test_cheats.c) keeps its own.
#include <stdint.h>
__attribute__((weak)) volatile uint64_t g_present_count;
__attribute__((weak)) const char *g_game_dir = "game";
