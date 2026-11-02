// A CPU without ARMv8.1's atomics gets an adapted copy of the game code library (runtime/gamelib_lse.c). Not in
// gamelib.h: that one is part of GAMELIB_ID, and this changes nothing in the library's interface.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// path: the library, changed to its adapted copy if this CPU needs one. false: why says what went wrong
bool gamelib_fit_cpu(char *path, size_t path_size, char *why, size_t why_size);
bool gamelib_cpu_lse(void);
#ifdef __cplusplus
}
#endif
