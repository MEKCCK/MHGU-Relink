// [Performance] Effects: the game's particles left out - hits' sparks, blood, fire, breath, dust, the rain, the snow,
// and the flashes that go with the monsters' and the Hunter Arts' attacks. They are drawn from three classes' draw
// entries; switched off, the entry returns without calling the game's draw, so none of it is submitted, while the
// effect itself (its sound, its timing, what it does to the game) still runs - the game's rules are untouched.
// The HUD's 2D effects (uEffect2D) and the arrows' and shots' models (uProofEffectModel) are left alone: the HUD stays
// as it is, and what is fired stays visible. Note that a monster's attack often warns through its effect: with this off
// the warning is gone too (the settings row says so).
// The three entries (main v1.4.0; the cUnit draw at vtable +0x30):
//   0xbba5b0 uEffect::draw, and through it uProofEffect's and uMHProofEffect's, and uProjectionEffect's own draw
//   0xe667a0 uSimpleEffect::draw
//   0xbb827c uCameraEffect::draw (the rain, the snow, the dust that follow the camera)
#include "../config.h"
#include "../hle.h"

#define FN_EFFECT        0xbba5b0u
#define FN_SIMPLE_EFFECT 0xe667a0u
#define FN_CAMERA_EFFECT 0xbb827cu

static void gate(Cpu *c, uint32_t entry) {
    if (config_live_effects()) fnhook_call_next(c, entry);
}
static void effect_gate(Cpu *c) { gate(c, FN_EFFECT); }
static void simple_effect_gate(Cpu *c) { gate(c, FN_SIMPLE_EFFECT); }
static void camera_effect_gate(Cpu *c) { gate(c, FN_CAMERA_EFFECT); }

void effects_init(void) {
    if (!fnhook_set_outer(FN_EFFECT, hle_register("effects:uEffect::draw", effect_gate)))
        LOG("[effects] the effect draw at %08x is not hooked\n", FN_EFFECT);
    if (!fnhook_set_outer(FN_SIMPLE_EFFECT, hle_register("effects:uSimpleEffect::draw", simple_effect_gate)))
        LOG("[effects] the simple effect draw at %08x is not hooked\n", FN_SIMPLE_EFFECT);
    if (!fnhook_set_outer(FN_CAMERA_EFFECT, hle_register("effects:uCameraEffect::draw", camera_effect_gate)))
        LOG("[effects] the camera effect draw at %08x is not hooked\n", FN_CAMERA_EFFECT);
}
