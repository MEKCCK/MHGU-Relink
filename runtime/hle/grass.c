// The field's scenery grass ([Performance] GrassOn). The game draws it from uGrass2::draw (main 0xb13490) once per
// update; the pass costs fill rate on a weak phone, so the draw is skipped while the setting is off. An outer hook
// (fnhook_set_outer): a mod that hooks the same function still gets the call when the gate lets it through, and the
// gate sees the mod's hook behind it either way.
#include "../config.h"
#include "../hle.h"

#define GRASS_DRAW_ENTRY 0xb13490u

static void grass_gate(Cpu *c) {
    if (config_grass_draw()) fnhook_call_next(c, GRASS_DRAW_ENTRY);
}

void grass_init(void) {
    if (!fnhook_set_outer(GRASS_DRAW_ENTRY, hle_register("grass:uGrass2::draw", grass_gate)))
        LOG("[grass] the field grass draw at %08x is not hooked\n", GRASS_DRAW_ENTRY);
}
