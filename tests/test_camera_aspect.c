// Exercise the actual camera hooks with an in-memory guest camera and projection output.
#include "rt.h"
#include <assert.h>
#include <math.h>
#include <string.h>

_Alignas(16) static unsigned char guest[0x1100];
#undef G2H
#define G2H(a) ((void *)(guest + (uint32_t)(a) - 0x10000u))
#include "../runtime/hle/camera_jitter.c"

float g_render_aspect;
Config g_config;
uint32_t g_render_size[2] = {3440, 1440}, g_ui_size[2] = {1720, 720};
volatile uint64_t g_present_count;
_Thread_local bool g_nvn_making_screenshot;

uint32_t hle_register(const char *name, HostFn fn) { (void)name; (void)fn; return 1; }
bool fnhook_set(uint32_t addr, uint32_t detour) { (void)addr; (void)detour; return true; }
void fnhook_call_original(Cpu *c, uint32_t fn) {
    float *m = G2H(c->r[0]);
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
    if (fn == FN_PROJECTION || fn == FN_EVENT_PROJECTION) {
        m[5] = 2;
        m[0] = m[5] / LDF(c->r[1] + CAM_ASPECT);
        m[11] = -1; m[15] = 0;
    }
}

int main(void) {
    g_render_aspect = 43.f / 18;
    camjit_init();
    Cpu c = {0};
    c.r[0] = 0x11000; c.r[1] = 0x10000;
    STF(c.r[1] + CAM_ASPECT, g_render_aspect);
    STF(c.r[1] + CAM_NEAR, .1f); STF(c.r[1] + CAM_FAR, 1000);
    const float ratios[] = {43.f/18, 16.f/9, 21.f/9, 64.f/27, 32.f/9, 16.f/10, 43.f/18};
    for (unsigned i = 0; i < sizeof ratios / sizeof *ratios; i++) {
        g_render_aspect = ratios[i];
        lookat(&c); projection(&c);
        CamRecord r;
        assert(camjit_latest(&r) && r.cam == c.r[1]);
        assert(fabsf(LDF(c.r[1] + CAM_ASPECT) - ratios[i]) < .0001f);
        assert(fabsf(r.proj[5] / r.proj[0] - ratios[i]) < .0001f);
        assert(r.proj[5] == 2); // Vertical scale stays fixed while horizontal view expands.
    }
    // A recreated camera can read the immutable boot canvas again after a live switch.
    g_render_aspect = 16.f/9;
    STF(c.r[1] + CAM_ASPECT, 43.f/18);
    event_lookat(&c); event_projection(&c);
    assert(fabsf(LDF(c.r[1] + CAM_ASPECT) - g_render_aspect) < .0001f);
    // Reusing that address for a narrow equipment-preview camera must not widen it.
    uint64_t captured = g_seq;
    STF(c.r[1] + CAM_ASPECT, .934f);
    lookat(&c); projection(&c);
    assert(LDF(c.r[1] + CAM_ASPECT) == .934f && g_seq == captured);
    puts("camera aspect: ultrawide boot, repeated live switches, capture and menu-preview isolation OK");
}
