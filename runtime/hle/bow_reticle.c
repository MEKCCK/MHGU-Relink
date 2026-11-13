// The native screen reticle while the local Bow is charging; this does not enter aiming mode.
// main v1.4.0 (fb08f1d20fd1204fe7070672777f869b): cUIOQuestScope::update calls its classifier
// with the hunter in r1, then category 8 asks the hunter's +0x1c8 virtual before drawing its own reticle.
// Other categories can bypass that gate, so both narrowly scoped UI calls must agree. The native hide
// branch hides nodes 3/5 every frame; no retained host state, aim flag, input, or camera write is needed.
#include "../hle.h"
#include "mousecam.h"

#define BOW_RT_CLASSIFY       0x81e438u
#define BOW_RT_CLASSIFY_LR    0x81e24cu
#define BOW_RT_AIM_MODE       0x4a1c48u
#define BOW_RT_AIM_MODE_LR    0x81e274u
#define BOW_RT_CHARGING       0x13b93dcu  // Bow virtual +0x694: pure act_st/act_no query
#define BOW_RT_DRAWN_FLAGS    0x2749u

static bool g_bow_reticle_ready;

static bool reticle_local_bow(uint32_t hunter) {
    return hunter && hunter == weapon_hunter() && weapon_ranged(hunter) == WEAPON_BOW;
}

static void reticle_classify(Cpu *c) {
    // Save the call's identity before a mod's next hook can change argument registers or LR.
    bool ours = g_bow_reticle_ready && c->r[14] == BOW_RT_CLASSIFY_LR && reticle_local_bow(c->r[1]);
    fnhook_call_next(c, BOW_RT_CLASSIFY);
    // Include a sheathed Bow: category 2 would otherwise bypass our false display gate.
    if (ours) c->r[0] = 8;
}

static void reticle_aim_mode(Cpu *c) {
    uint32_t hunter = c->r[0];
    bool ours = g_bow_reticle_ready && c->r[14] == BOW_RT_AIM_MODE_LR && reticle_local_bow(hunter);
    fnhook_call_next(c, BOW_RT_AIM_MODE);
    if (!ours) return;
    if (!(LD8(hunter + BOW_RT_DRAWN_FLAGS) & 1)) { c->r[0] = 0; return; }
    // guest_call only preserves LR. Query a full copy so native/plugin register and VFP results survive.
    Cpu query = *c;
    query.r[0] = hunter;
    guest_call(&query, BOW_RT_CHARGING);
    c->r[0] = query.r[0] != 0;
}

void bow_reticle_init(void) {
    bool classify = fnhook_set_outer(BOW_RT_CLASSIFY, hle_register("bow_reticle:classify", reticle_classify));
    bool aim_mode = fnhook_set_outer(BOW_RT_AIM_MODE, hle_register("bow_reticle:aim_mode", reticle_aim_mode));
    // If either installation failed, an installed half continues the existing hook chain unchanged.
    g_bow_reticle_ready = classify && aim_mode;
    if (!g_bow_reticle_ready) LOG("[bow_reticle] disabled: native screen-reticle hooks %d/%d\n", classify, aim_mode);
}
