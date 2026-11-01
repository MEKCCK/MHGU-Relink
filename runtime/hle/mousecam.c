// Mouse look on the hunt camera's own angles, not through the right stick. The stick turns the camera at a speed
// (past a round dead zone of about 39% tilt, faster with more tilt), so a mouse fed into it can't make small precise
// turns; here each count moved turns the camera by a fixed angle (MOUSECAM_DEG_PER_COUNT x [Mouse]
// MouseSensitivity), as in a PC game.
//
// The quest camera (uQuestCamera, 0xb00 bytes) keeps its angles as 16-bit units (0x10000 = a full turn): +0x9e8 /
// +0x9ec the pitch / yaw it heads for, +0x9e0 / +0x9e4 where it is (smoothed toward those each update), +0x9d8 /
// +0x9dc the angles the eye is placed by (the smoothed ones, the pitch then lifted off the ground). The stick handler
// (main+0x255CC; r0 = the camera, r1 = the game's invert option: bit 0 up / down, bit 1 left / right, from options
// byte +0x64, copied to +0x97c, or +0x980's for the second camera type) adds the stick's turn to +0x9e8 / +0x9ec,
// pitch clamped to +-0x3f00 (89 degrees). The game calls it only while the player may turn the camera (not in
// cutscenes, menus, while carving...), so it is hooked: after it, the mouse's movement since the last update goes to
// both the heading and the smoothed angles (no smoothing lag), with the game's invert option.
// Where it doesn't run (villages, whose camera doesn't turn, or other cameras) host_input.c gives the mouse to the
// right stick as before (mousecam_active).
//
// The lock-on (runtime/touch.cpp: the target icon, a long press on L, a controller's right stick click): in the same
// hook, the camera turns toward the nearest large monster every update, eased, as in the newer games. The camera's
// yaw unit u looks along (x, z) = (-sin, -cos)(u * 2 pi / 0x10000) (measured on a Galaxy S25: the eye +0x40 toward the
// target point +0x60); its pitch is positive looking up. Monsters: sEnemy (0x1a51fe0) +0x1c, ten slots; each
// one's status block (+0x1428) has its place at +0x40 and a network id at +0x1b6 (a large monster's below 10000).
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#include "../config.h"
#include "../hle.h"
#include "mousecam.h"

#define FN_CAMERA_STICK 0x2255ccu  // uQuestCamera's right stick handler: r0 = the camera, r1 = the invert bits
#define QC_PITCH_NOW    0x9e0u
#define QC_YAW_NOW      0x9e4u
#define QC_PITCH_TO     0x9e8u
#define QC_YAW_TO       0x9ecu
#define PITCH_MAX       0x3f00     // (the stick handler's clamp)
#define MOUSECAM_DEG_PER_COUNT 0.08f
#define ACTIVE_NS 200000000ull     // the handler ran this recently: the mouse is the camera's

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_dx, g_dy;           // counts moved since the handler last ran (x right, y down)
static volatile uint64_t g_seen_ns;  // when the handler last ran

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

bool mousecam_active(void) {
    uint64_t seen = g_seen_ns;
    return seen && plat_now_ns() - seen < ACTIVE_NS;
}

void mousecam_add(float dx, float dy) {
    pthread_mutex_lock(&g_lock);
    g_dx += dx, g_dy += dy;
    pthread_mutex_unlock(&g_lock);
}

static int clamp_pitch(int p) { return p < -PITCH_MAX ? -PITCH_MAX : p > PITCH_MAX ? PITCH_MAX : p; }

bool touch_lock_active(void);  // (runtime/touch.cpp: the lock-on is on and the player is not turning the camera)
#define G_SENEMY 0x1a51fe0u
#define LOCK_TAU 0.12f       // seconds for the camera to turn ~63% of the way to the monster (eased)
#define LOCK_RANGE 6000.0f   // (a monster farther away from the hunter than this is not followed)
#define LOCK_HEIGHT 90.0f    // (the camera aims this far above the monster's feet)
#define LOCK_NEAR0 120.0f    // closer than this (under the monster) the camera holds its heading,
#define LOCK_NEAR1 380.0f    // farther than this the full follow (eased between)

// The large monster to follow: the one followed already, unless another is clearly nearer (30%: no back and forth
// between two at about the same distance), else the nearest; its place, or false. (the hunt camera's thread alone)
static uint32_t g_lock_en;
static bool lock_target(float x, float y, float z, float out[3]) {
    (void)y;
    uint32_t sen = LD32(G_SENEMY);
    if (!sane(sen)) return false;
    float best = LOCK_RANGE * LOCK_RANGE, cur_d = -1.0f, cur[3] = {0};
    uint32_t best_en = 0;
    for (int k = 0; k < 10; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info = sane(en) ? LD32(en + 0x1428) : 0;
        if (!sane(info)) continue;
        uint32_t id = LD16(info + 0x1b6);
        if (id == 0xffff || id >= 10000) continue;
        float mx = LDF(info + 0x40), my = LDF(info + 0x44), mz = LDF(info + 0x48);
        if (!isfinite(mx) || !isfinite(my) || !isfinite(mz)) continue;
        float d = (mx - x) * (mx - x) + (mz - z) * (mz - z);
        if (en == g_lock_en && d < LOCK_RANGE * LOCK_RANGE) cur_d = d, cur[0] = mx, cur[1] = my, cur[2] = mz;
        if (d < best) best = d, best_en = en, out[0] = mx, out[1] = my, out[2] = mz;
    }
    if (cur_d >= 0.0f && best_en != g_lock_en && best >= cur_d * 0.49f) {
        out[0] = cur[0], out[1] = cur[1], out[2] = cur[2];
        return true;
    }
    g_lock_en = best_en;
    return best_en != 0;
}

// one update of the lock-on: the camera's heading and its smoothed angles eased toward the monster
static void lock_follow(uint32_t cam, float dt) {
    float t[3];
    float hx = LDF(cam + 0x60), hy = LDF(cam + 0x64), hz = LDF(cam + 0x68);  // (the target point: by the hunter)
    if (!isfinite(hx) || !lock_target(hx, hy, hz, t)) return;
    float dx = t[0] - hx, dz = t[2] - hz, dist = sqrtf(dx * dx + dz * dz);
    // under the monster (its root's bearing swings with every step): the follow fades out, the camera holds
    float f = (dist - LOCK_NEAR0) / (LOCK_NEAR1 - LOCK_NEAR0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    float k = (1.0f - expf(-dt / LOCK_TAU)) * f * f * (3.0f - 2.0f * f);
    if (k <= 0.0f) return;
    const float U = 65536.0f / (2.0f * 3.14159265f);
    int want = (int)lrintf(atan2f(-dx, -dz) * U) & 0xffff;
    int yaw_to = (int)(LD32(cam + QC_YAW_TO) & 0xffff), dy = (int16_t)(uint16_t)(want - yaw_to);
    int step = (int)lrintf(dy * k);
    if (!step && dy && k > .05f) step = dy > 0 ? 1 : -1;
    if (step) {
        ST32(cam + QC_YAW_TO, (uint32_t)(yaw_to + step) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)step) & 0xffffu);
    }
    // the pitch: toward the monster's body, gently and within a comfortable range (looking a little down)
    float rise = (t[1] + LOCK_HEIGHT) - hy;
    int pwant = (int)lrintf(atan2f(rise, dist) * U);
    if (pwant > 0x0c00) pwant = 0x0c00;
    if (pwant < -0x0e00) pwant = -0x0e00;
    int pto = (int16_t)LD32(cam + QC_PITCH_TO), pstep = (int)lrintf((pwant - pto) * k * 0.6f);
    if (pstep) {
        int next = clamp_pitch(pto + pstep), now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + pstep);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
    }
}

// (debug) MHGU_CAMLOG=1: twice a second, the hunt camera (eye, target, its angles) and the large monsters' places
static void camlog(uint32_t cam) {
    static int on = -1;
    static uint64_t next;
    if (on < 0) on = getenv("MHGU_CAMLOG") != NULL;
    uint64_t now = plat_now_ns();
    if (!on || now < next || !sane(cam)) return;
    next = now + 500000000ull;
    LOG("[camlog] cam %08x eye %.1f %.1f %.1f tgt %.1f %.1f %.1f | pitch now %d to %d yaw now %d to %d\n", cam,
        LDF(cam + 0x40), LDF(cam + 0x44), LDF(cam + 0x48), LDF(cam + 0x60), LDF(cam + 0x64), LDF(cam + 0x68),
        (int16_t)LD32(cam + QC_PITCH_NOW), (int16_t)LD32(cam + QC_PITCH_TO), (int)(LD32(cam + QC_YAW_NOW) & 0xffff),
        (int)(LD32(cam + QC_YAW_TO) & 0xffff));
    uint32_t sen = LD32(0x1a51fe0u);
    if (!sen) return;
    for (int k = 0; k < 10; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info = en ? LD32(en + 0x1428) : 0;
        if (!info) continue;
        uint32_t id = LD16(info + 0x1b6);
        LOG("[camlog]   enemy %d %08x id %u at %.1f %.1f %.1f | zone %u | +0x370 %d +0x374 %d%s\n", k, en, id,
            LDF(info + 0x40), LDF(info + 0x44), LDF(info + 0x48), LD8(en + 0x1054), (int32_t)LD32(info + 0x370),
            (int32_t)LD32(info + 0x374), en == g_lock_en ? " (followed)" : "");
    }
}

static void camera_stick(Cpu *c) {
    uint32_t cam = c->r[0], invert = c->r[1];
    fnhook_call_original(c, FN_CAMERA_STICK);
    camlog(cam);
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    g_seen_ns = now;
    pthread_mutex_lock(&g_lock);
    float dx = g_dx, dy = g_dy;
    g_dx = g_dy = 0;
    pthread_mutex_unlock(&g_lock);
    if (sane(cam) && seen && now - seen < ACTIVE_NS && touch_lock_active()) {
        float dt = (now - seen) / 1e9f;
        lock_follow(cam, dt > 0.1f ? 0.1f : dt);
    } else {
        g_lock_en = 0;
    }
    static float rest_yaw, rest_pitch;  // (the fractions of a unit left over, so slow movements add up)
    if (!seen || now - seen >= ACTIVE_NS) rest_yaw = rest_pitch = 0, dx = dy = 0;  // (moved while it didn't run)
    if ((dx == 0 && dy == 0) || !sane(cam)) return;
    float s = MOUSECAM_DEG_PER_COUNT * (65536.0f / 360.0f) * config_live_mouse_sensitivity();
    float yaw = -dx * s, pitch = -dy * s;  // (right turns the yaw down, up raises the pitch: as the stick)
    if (invert & 1) pitch = -pitch;
    if (invert & 2) yaw = -yaw;
    yaw += rest_yaw, pitch += rest_pitch;
    int iy = (int)yaw, ip = (int)pitch;
    rest_yaw = yaw - (float)iy, rest_pitch = pitch - (float)ip;
    if (iy) {
        ST32(cam + QC_YAW_TO, (LD32(cam + QC_YAW_TO) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)iy) & 0xffffu);
    }
    if (ip) {
        int to = (int16_t)LD32(cam + QC_PITCH_TO), next = clamp_pitch(to + ip);
        int now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + next - to);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
        if (next != to + ip) rest_pitch = 0;  // (at the limit)
    }
}

void mousecam_init(void) {
    fnhook_set(FN_CAMERA_STICK, hle_register("mousecam:camera_stick", camera_stick));
}
