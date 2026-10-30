// Typed chat anywhere (runtime/hle/chat.c): "/" opens a line to type; Enter sends it as the game sends a shoutout.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Cpu;
void chat_open(void);                          // window side: "/" pressed (no text entry or menu open)
void chat_close(void);                         // the game opened its own text entry
bool chat_composing(char *text, size_t size);  // the overlay: the line being typed
void chat_tick(struct Cpu *c);                 // game thread, each frame (its input polling): send what was typed

#ifdef __cplusplus
}
#endif
