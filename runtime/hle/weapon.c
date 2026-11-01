// Whether the local hunter has the weapon out (the touch controls: running with the thumb past the stick's rim is R,
// which with the weapon out is some weapons' guard: runtime/touch.cpp runs only sheathed).
// main v1.4.0: the game's own question is a virtual of the hunter, slot 94, FN_DRAWN (0x4a2444): bit 0 of the byte
// +0x2749. The hunter has a class per weapon (0x1949a88..0x19515ec, on 0x19491bc) and each of them has that same
// function in that slot, so the slot tells a hunter of any weapon, and nothing else needs to know which one it is.
// The local player's slot (sGameControl *0x1a8ca48 +0x30 its number; sPlayer *0x1a85b58 +0x1c the 4 slots) has held
// the hunter, or on some hunts an object of another class (vtable 0x198bf48, no place in the world) that points to the
// hunter at +0x15b0 or +0x3010: both seen on a Galaxy S25. No fixed address or layout of a session is kept here.
#include "../hle.h"

#define G_SPLAYER     0x1a85b58u
#define G_GAMECONTROL 0x1a8ca48u
#define VT_DRAWN      94u          // the slot of the vtable
#define FN_DRAWN      0x4a2444u
#define PL_FLAGS      0x2749u      // (bit 0: the weapon is out, drawing included)
#define LINK_A        0x15b0u
#define LINK_B        0x3010u
#define MAIN_LO       0x00200000u  // (main's image: a vtable is in it)
#define MAIN_HI       0x02347000u

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static bool is_hunter(uint32_t p) {
    if (!sane(p)) return false;
    uint32_t vt = LD32(p);
    return vt >= MAIN_LO && vt < MAIN_HI && !(vt & 3) && LD32(vt + 4 * VT_DRAWN) == FN_DRAWN;
}

// 1 the weapon is out, 0 sheathed, -1 not known (no hunter: a menu, a town's loading, a Prowler)
int weapon_drawn(void) {
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    if (!sane(gc) || !sane(sp)) return -1;
    uint32_t slot = LD32(gc + 0x30);
    if (slot > 3) return -1;
    uint32_t pl = LD32(sp + 0x1c + 4u * slot), h = 0;
    if (!sane(pl)) return -1;
    if (is_hunter(pl)) h = pl;
    else if (is_hunter(LD32(pl + LINK_A))) h = LD32(pl + LINK_A);
    else if (is_hunter(LD32(pl + LINK_B))) h = LD32(pl + LINK_B);
    static uint32_t seen_pl = 1, seen_h = 1;
    if ((pl != seen_pl || h != seen_h) && ENV("MHGU_TOUCH_LOG"))
        LOG("[weapon] player slot %08x (vtable %08x): hunter %08x\n", pl, LD32(pl), h);
    seen_pl = pl, seen_h = h;
    if (!h) return -1;
    return LD8(h + PL_FLAGS) & 1;
}
