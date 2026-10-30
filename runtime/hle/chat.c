// Typed chat anywhere. The game only lets you type in the Hub (Minus opens the keyboard; in a quest Minus is the kick),
// but a shoutout in a quest is sent as its text: one netChatTextData (0xac bytes: u8 type at +9, the sender's name
// at +0xa, UTF-8 text at +0x32), the same message as Hub chat (type 1 + the sender's player index; a shoutout 6 +
// index), shown in every player's chat window as it arrives. So "/" opens a line to type (runtime/textinput.c, drawn
// by the overlay) and Enter sends it the way the game sends a shoutout, from the game's thread.
#include "chat.h"

#include <stdatomic.h>
#include <string.h>

#include "../hle.h"
#include "../textinput.h"

// Game code and data (main, v1.4.0; runtime addresses). Found from the Hub chat's own sender (0x870270).
#define FN_SEND_CHAT   0x41e204u  // (net object, type, name, text[0x78], int on the stack (1)): builds and sends it
#define FN_NET_INDEX   0x427ae4u  // (sNetwork) -> our player index in the session
#define FN_NET_ONLINE  0x427ba8u  // (sNetwork) -> in a session (else the index is 0)
#define FN_NET_OBJECT  0x427b3cu  // (sNetwork, 0x6b) -> the object chat messages are sent through
#define FN_MY_PLAYER   0x476278u  // (sPlayer) -> our player; +0x13c4 -> its data, +0x514 there: the hunter's name
#define G_SNETWORK     0x1a843f0u
#define G_SPLAYER      0x1a85b58u
#define CHAT_TYPE      6u         // + player index: a shoutout's type (shown in quests too)
#define TEXT_BYTES     0x78u      // what the sender copies (the game's own texts are at most 0x68 bytes)
#define MAX_CHARS      26         // as the Hub's chat entry
#define MAX_BYTES      0x67

static atomic_bool g_composing;

void chat_open(void) {
    if (atomic_exchange(&g_composing, true)) return;
    textinput_begin("", MAX_CHARS);
}

void chat_close(void) { atomic_store(&g_composing, false); }

bool chat_composing(char *text, size_t size) {
    if (!atomic_load(&g_composing)) return false;
    textinput_peek(text, size);
    return true;
}

static uint32_t call(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    return c->r[0];
}

static void send_text(Cpu *c, const char *text) {
    uint32_t net = LD32(G_SNETWORK), players = LD32(G_SPLAYER);
    if (!net || !players) { LOG("[chat] not sent: no network or player yet\n"); return; }
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    int32_t index = (int32_t)call(c, FN_NET_INDEX, net, 0, 0, 0);
    if (!call(c, FN_NET_ONLINE, net, 0, 0, 0)) index = 0;
    uint32_t object = index < 0 ? 0 : call(c, FN_NET_OBJECT, net, 0x6b, 0, 0);
    uint32_t me = call(c, FN_MY_PLAYER, players, 0, 0, 0), data = me ? LD32(me + 0x13c4) : 0;
    if (!object || !data) {
        LOG("[chat] not sent: %s\n", !object ? "no chat connection" : "no hunter");
    } else {
        uint32_t sp = (saved[13] - 0x100) & ~7u, buf = sp + 0x10;  // (the text and the stack argument, below the frame)
        memset(G2H(buf), 0, TEXT_BYTES);
        memcpy(G2H(buf), text, strlen(text));
        ST32(sp, 1);
        c->r[13] = sp;
        call(c, FN_SEND_CHAT, object, CHAT_TYPE + (uint32_t)index, data + 0x514, buf);
        LOG("[chat] sent \"%s\"\n", text);
    }
    memcpy(c->r, saved, sizeof saved);
}

void chat_tick(Cpu *c) {
    if (!atomic_load(&g_composing)) return;
    char text[256];
    int chars, ev = textinput_poll(text, sizeof text, &chars);
    if (ev != TI_ENTER && ev != TI_CANCEL) return;
    textinput_end();
    atomic_store(&g_composing, false);
    if (ev == TI_CANCEL) return;
    size_t n = strlen(text);
    while (n > MAX_BYTES) {  // (whole characters only)
        n--;
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    text[n] = 0;
    if (n) send_text(c, text);
}
