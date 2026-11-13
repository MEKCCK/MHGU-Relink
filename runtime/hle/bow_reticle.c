// Bow charge uses the native world-space guide, not cUIOQuestScope's screen reticle.
// main v1.4.0 (fb08f1d20fd1204fe7070672777f869b): uShellPlEffectW10 effect 4 asks
// 0x13b7750 at LR 0x657690 before updating its bone position, aim rotation and visibility.
// Keep the separate arc-shot landing markers, manual aiming and camera queries native.
#include "../hle.h"
#include "mousecam.h"

#define BOW_GUIDE_AIM         0x13b7750u
#define BOW_GUIDE_AIM_LR      0x657690u
#define BOW_GUIDE_CHARGING    0x13b93dcu  // pure act_st/act_no query, Bow virtual +0x694
#define BOW_GUIDE_DRAWN_FLAGS 0x2749u

static void bow_guide_visible(Cpu *c) {
    uint32_t hunter = c->r[0];
    bool ours = c->r[14] == BOW_GUIDE_AIM_LR && hunter &&
                hunter == weapon_hunter() && weapon_ranged(hunter) == WEAPON_BOW;
    fnhook_call_next(c, BOW_GUIDE_AIM);
    // Preserve manual/plugin visibility; only add the local charging guide.
    if (!ours || c->r[0] || !(LD8(hunter + BOW_GUIDE_DRAWN_FLAGS) & 1)) return;
    // guest_call preserves only LR; isolate the query from the next hook's CPU/VFP result.
    Cpu query = *c;
    query.r[0] = hunter;
    guest_call(&query, BOW_GUIDE_CHARGING);
    if (query.r[0]) c->r[0] = 1;
}

void bow_reticle_init(void) {
    if (!fnhook_set_outer(BOW_GUIDE_AIM, hle_register("bow_guide:visible", bow_guide_visible)))
        LOG("[bow_guide] disabled: native charge-guide hook unavailable\n");
}
