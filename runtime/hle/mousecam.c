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
//
// While aiming (the Bow's R, a Bowgun's aim...) the stick turns the hunter instead and the camera follows: the
// handler isn't called, the hunter's turn is applied by 0x4a1738 (r0 = the player, r1 = which weapon's aim), to the
// hunter's yaw +0x14a0 and aim pitch +0x14b8 / +0x14bc (a float, -100 to 100; +0x2714, 16-bit units, for mode 1). The
// aim camera (0x227870) takes +0x96c from that yaw and +0x968 from that pitch, spread over its aim limits (camera
// parameters 0x1d up and 0x1e down, degrees), and steps its own eye angles toward them. So 0x4a1738 is hooked
// too, gated by the native aiming query and with the aiming invert option (options byte +0x66).
//
// Where neither runs (villages, whose camera doesn't turn, or other cameras) host_input.c gives the mouse to the
// right stick as before (mousecam_active).
//
// The lock-on (runtime/touch.cpp: the target icon, a long press on L, a controller's right stick click): in the same
// hook, the camera turns toward the nearest large monster every update, eased, as in the newer games. The camera's
// yaw unit u looks along (x, z) = (-sin, -cos)(u * 2 pi / 0x10000) (measured on a Galaxy S25: the eye +0x40 toward the
// target point +0x60); its pitch is positive looking up. Only a monster in the hunter's zone of the map counts (+0x1054
// of each: the game's own "(Diff Area)"), or the camera would stare at a wall. Monsters: sEnemy (0x1a51fe0) +0x1c, ten slots; each
// one's status block (+0x1428) has its place at +0x40 and a network id at +0x1b6 (a large monster's below 10000).
//
// A shot with the lock-on on goes to the monster: arrows and shells leave along the hunter's heading (hunter +0x14a0, in
// the camera's units; (x, z) = (sin, cos); written, the hunter turns at once; +0xfec and +0x2ee4 are copies of it; its
// place is at +0xa0; measured on a Motorola g06). An unaimed fire press turns toward the monster for that shot;
// manual aiming retains the player's direction unless PersistentLockOn is enabled.
//
// A bow's charge from normal stance is redirected until its release. Bowguns redirect only unaimed shots;
// their unaimed shells stay level. Without persistent tracking, manual aim and styles keep their own direction.
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#include "../config.h"
#include "../hle.h"
#include "input.h"
#include "mousecam.h"

#define FN_CAMERA_STICK 0x2255ccu  // uQuestCamera's right stick handler: r0 = the camera, r1 = the invert bits
#define FN_AIM_TURN     0x4a1738u  // the hunter's turn while aiming: r0 = the player, r1 = the mode
#define FN_AIMING       0x4a05b8u  // the aim camera's own action query: (hunter, 1, 0), not merely an aim-turn call
#define FN_CAM_PARAM    0x21eadcu  // (camera, parameter): s0 = the camera's parameter
#define G_OPTIONS       0x1a8ca48u  // the game's options object *
#define OPT_AIM_INVERT  0x66u      // options byte: the invert bits while aiming
#define QC_PLAYER       0x85cu
#define QC_EYE_PITCH    0x9d8u
#define QC_EYE_YAW      0x9dcu
#define QC_PITCH_NOW    0x9e0u
#define QC_YAW_NOW      0x9e4u
#define QC_PITCH_TO     0x9e8u
#define QC_YAW_TO       0x9ecu
#define QC_PARAM_AIM_UP   0x1du
#define QC_PARAM_AIM_DOWN 0x1eu
#define PL_AIM_YAW      0x14a0u
#define PL_AIM_PITCH    0x14b8u    // (the float, and +4 the copy the camera reads)
#define PL_AIM_PITCH2   0x14bcu
#define PL_AIM_LEAN     0x2714u    // (mode 1's pitch: 16-bit units, +-AIM_LEAN_MAX)
#define AIM_LEAN_MAX    0x2000
#define AIM_PITCH_MAX   100.0f
#define UNITS_PER_DEG   (65536.0f / 360.0f)
#define UNITS_PER_RAD   (65536.0f / (2.0f * 3.14159265f))
#define PITCH_MAX       0x3f00     // (the stick handler's clamp)
#define MOUSECAM_DEG_PER_COUNT 0.08f
#define ACTIVE_NS 200000000ull     // the handler ran this recently: the mouse is the camera's

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_dx, g_dy;           // counts moved since the handler last ran (x right, y down)
static float g_mot_yaw, g_mot_pitch;  // radians the phone turned since then (see mousecam_motion_add; yaw right, pitch up)
static int g_cycle_req;            // (host thread) lock-on targets asked for, taken one per hunt camera update
static volatile uint64_t g_seen_ns;  // when a handler last ran
static float g_rest_yaw, g_rest_pitch;  // (the fractions of a unit left over, so slow movements add up)
static float g_trail_dx, g_trail_dy;    // (counts a smoothing setting still holds back)
static int g_log_on = -1;               // MHGU_MOUSECAM_LOG=1: every turn applied, with the camera's angles
static bool g_last_aim;                 // (the last turn went to the aiming hunter)
static uint32_t g_cam;             // (the game's thread) the camera the stick handler last had
static uint32_t g_stick_runs;      // (the game's thread) how many times it ran
static uint32_t g_aim_hunter, g_aim_action;
static uint64_t g_aim_seen_ns;

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

bool mousecam_active(void) {
    uint64_t seen = g_seen_ns;
    return seen && plat_now_ns() - seen < ACTIVE_NS;
}

bool mousecam_hunter_aiming(uint32_t hunter) {
    uint64_t seen = __atomic_load_n(&g_aim_seen_ns, __ATOMIC_ACQUIRE);
    if (!seen || !hunter || hunter != __atomic_load_n(&g_aim_hunter, __ATOMIC_RELAXED) ||
        plat_now_ns() - seen >= 150000000ull) return false;
    unsigned action = weapon_action(hunter);
    // Recoil keeps the scope open briefly without calling the aim handler. A common idle/roll/knockback ends it.
    return action == __atomic_load_n(&g_aim_action, __ATOMIC_RELAXED) || (action >> 16 & 0x7fff) == 0x10;
}

void mousecam_add(float dx, float dy) {
    pthread_mutex_lock(&g_lock);
    g_dx += dx, g_dy += dy;
    pthread_mutex_unlock(&g_lock);
}

void mousecam_motion_add(float yaw, float pitch) {
    if (!isfinite(yaw) || !isfinite(pitch)) return;
    pthread_mutex_lock(&g_lock);
    g_mot_yaw += yaw, g_mot_pitch += pitch;
    pthread_mutex_unlock(&g_lock);
}

void mousecam_motion_reset(void) {
    pthread_mutex_lock(&g_lock);
    g_mot_yaw = g_mot_pitch = 0.0f;
    pthread_mutex_unlock(&g_lock);
}

void mousecam_target_cycle(void) {
    pthread_mutex_lock(&g_lock);
    if (g_cycle_req < 8) g_cycle_req++;  // (no runaway: a queued tap each is a target)
    pthread_mutex_unlock(&g_lock);
}

static int clamp_pitch(int p) { return p < -PITCH_MAX ? -PITCH_MAX : p > PITCH_MAX ? PITCH_MAX : p; }

bool touch_lock_active(void);  // (runtime/touch.cpp: the lock-on is on and the player is not turning the camera)
bool touch_lock_on(void);      // (the lock-on is on, whatever the camera is doing)
#define ZONE       0x1054u       // (u8, of a hunter and of a monster: which zone of the map it is in)
#define MUZZLE_Y   110.0f        // (a shot leaves this far above the hunter's feet)
#define PL_PLACE   0xa0u
#define SHOT_NS    400000000ull  // (the fire button let go: the shot may leave a little later, on a bow always)
#define LOCK_MOVE_DEADZONE 3000  // match host_input.c's per-axis gamepad drift filter
#define G_SENEMY 0x1a51fe0u
#define LOCK_TAU 0.12f       // seconds for the camera to turn ~63% of the way to the monster (eased)
#define LOCK_RANGE 6000.0f   // (a monster farther away from the hunter than this is not followed)
#define LOCK_HEIGHT 90.0f    // (the camera aims this far above the monster's feet)
#define LOCK_NEAR0 120.0f    // closer than this (under the monster) the camera holds its heading,
#define LOCK_NEAR1 380.0f    // farther than this the full follow (eased between)
// A model's skeleton (the hunter's and every monster's, measured on a Motorola g06 in the arena): +0x490 the number of
// joints, +0x494 their array, 0xa0 bytes each (+0x00 a vtable in main's image, +0x10 the world matrix's rows, its
// translation at +0x40 with w = 1.0, +0x60 the rotation, +0x70 the scale, +0x91 the parent: 0xff the root). The root
// is the pelvis, the body's middle (a quadruped's at 190 of the 215 its back's joints reach; its feet at 0).
#define EN_JOINTS   0x490u
#define JOINT_SIZE  0xa0u
#define JOINT_POS   0x40u

// The large monster the lock-on follows, remembered by its pointer and the id the game gave it: kept while it is
// still a valid one in the hunter's zone, else the nearest. The host's target control (mousecam_target_cycle) turns
// to the next valid one in slot order, around, one request per update. (the hunt camera's thread alone)
static uint32_t g_lock_en, g_target_id, g_target_hunter;
static int g_lock_slot = -1;
static uint64_t g_lock_cycle_ns;  // (when a request was last taken: the two hooks of one update take only one)

// A large monster the lock-on may follow: sEnemy slot k, the monster and its id, where it is, how far
typedef struct {
    uint32_t en, id;
    float x, y, z, d2;
    int slot;
} LockChoice;

// The valid monsters around (x, z), in slot order: sEnemy's ten slots with a live large monster (a network id below
// 10000) in the hunter's zone, a finite place, and no farther than LOCK_RANGE. Returns how many (at most 10).
static int lock_choices(LockChoice out[10], uint32_t hunter, float x, float z) {
    uint32_t sen = LD32(G_SENEMY);
    if (!sane(sen)) return 0;
    int n = 0;
    for (int k = 0; k < 10; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info = sane(en) ? LD32(en + 0x1428) : 0;
        if (!sane(info)) continue;
        uint32_t id = LD16(info + 0x1b6);
        if (id == 0xffff || id >= 10000) continue;
        if (hunter && LD8(en + ZONE) != LD8(hunter + ZONE)) continue;  // (not through the map's walls)
        float mx = LDF(info + 0x40), my = LDF(info + 0x44), mz = LDF(info + 0x48);
        if (!isfinite(mx) || !isfinite(my) || !isfinite(mz)) continue;
        float d2 = (mx - x) * (mx - x) + (mz - z) * (mz - z);
        if (d2 > LOCK_RANGE * LOCK_RANGE) continue;
        LockChoice *c = &out[n++];
        c->en = en, c->id = id, c->x = mx, c->y = my, c->z = mz, c->d2 = d2, c->slot = k;
    }
    return n;
}

// This update's monster and its place, or false. update: take at most one of the host's cycle requests (the camera's
// and the hunter's hooks both ask, once an update; the 4 ms gate keeps one tap from counting twice in one update).
static bool lock_target(float x, float y, float z, float out[3], bool update) {
    (void)y;
    uint32_t hunter = weapon_hunter();
    LockChoice c[10];
    int n = lock_choices(c, hunter, x, z), cur = -1;
    if (hunter != g_target_hunter) g_lock_en = g_target_id = 0, g_lock_slot = -1, g_target_hunter = hunter;
    for (int i = 0; i < n && g_lock_en; i++)
        if (c[i].en == g_lock_en && c[i].id == g_target_id) cur = i;
    bool cycle = false;
    if (update) {
        uint64_t now = plat_now_ns();
        pthread_mutex_lock(&g_lock);
        if (g_cycle_req > 0 && now - g_lock_cycle_ns >= 4000000ull)  // (one a game update: its two hooks are one)
            g_cycle_req--, g_lock_cycle_ns = now, cycle = true;
        pthread_mutex_unlock(&g_lock);
    }
    if (n == 0) {  // (no monster to follow now: the next hunt starts over)
        g_lock_en = g_target_id = 0, g_lock_slot = -1;
        return false;
    }
    if (cycle) {  // the next one in slot order, around; the followed one gone, the first after its slot
        int next = cur >= 0 ? (cur + 1) % n : 0;
        if (cur < 0)
            for (int i = 0; i < n; i++)
                if (c[i].slot > g_lock_slot) { next = i; break; }
        cur = next;
    } else if (cur < 0) {  // (none followed yet, or it went away: the nearest)
        cur = 0;
        for (int i = 1; i < n; i++)
            if (c[i].d2 < c[cur].d2) cur = i;
    }
    g_lock_en = c[cur].en, g_target_id = c[cur].id, g_lock_slot = c[cur].slot;
    out[0] = c[cur].x, out[1] = c[cur].y, out[2] = c[cur].z;
    return true;
}

// Where a locked shot goes: the monster's body, not its feet. The root joint (the pelvis) when it is well off the
// ground, else the middle of the skeleton's box (a monster whose root is low); false: no skeleton to read (its feet
// and a fixed height then)
static bool body_center(uint32_t en, float feet_y, float out[3]) {
    if (!sane(en)) return false;
    uint32_t n = LD32(en + EN_JOINTS), arr = LD32(en + EN_JOINTS + 4);
    if (n < 3 || n > 512 || !sane(arr)) return false;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, root[3] = {0};
    bool have_root = false;
    int good = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = arr + i * JOINT_SIZE, vt = LD32(j);
        if (vt < 0x00200000u || vt >= 0x02347000u) break;  // (not a joint: the array is not what it was measured to be)
        float p[3] = {LDF(j + JOINT_POS), LDF(j + JOINT_POS + 4), LDF(j + JOINT_POS + 8)};
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || LDF(j + JOINT_POS + 12) != 1.0f) continue;
        for (int k = 0; k < 3; k++) lo[k] = fminf(lo[k], p[k]), hi[k] = fmaxf(hi[k], p[k]);
        if (!have_root && LD8(j + 0x91) == 0xff) root[0] = p[0], root[1] = p[1], root[2] = p[2], have_root = true;
        good++;
    }
    if (good < 3 || hi[1] - lo[1] < 1.0f || hi[1] - feet_y > 5000.0f) return false;
    if (have_root && root[1] - lo[1] > 0.35f * (hi[1] - lo[1])) {
        out[0] = root[0], out[1] = root[1], out[2] = root[2];
    } else {
        for (int k = 0; k < 3; k++) out[k] = 0.5f * (lo[k] + hi[k]);
    }
    return true;
}

// one update of the lock-on: the camera's heading and its smoothed angles eased toward the monster
static void lock_follow(uint32_t cam, float dt) {
    float t[3];
    float hx = LDF(cam + 0x60), hy = LDF(cam + 0x64), hz = LDF(cam + 0x68);  // (the target point: by the hunter)
    if (!isfinite(hx) || !lock_target(hx, hy, hz, t, true)) return;
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

static bool left_stick_active(const int32_t sticks[4]) {
    return sticks[0] <= -LOCK_MOVE_DEADZONE || sticks[0] >= LOCK_MOVE_DEADZONE ||
           sticks[1] <= -LOCK_MOVE_DEADZONE || sticks[1] >= LOCK_MOVE_DEADZONE;
}

static uint64_t lock_cancel_buttons(int ranged) {
    return (1ull << BTN_B) | (1ull << BTN_Y) | (1ull << (ranged == WEAPON_BOW ? BTN_A : BTN_X));
}

// Both hooks see the same explicit cancellation; native aiming alone must not block persistent scoped fire.
static bool lock_canceled(uint32_t hunter, int ranged, uint64_t buttons) {
    static uint32_t previous_hunter, previous_weapon;
    static bool held;
    uint32_t weapon = hunter ? LD32(hunter) : 0;
    if (hunter != previous_hunter || weapon != previous_weapon || !ranged)
        previous_hunter = hunter, previous_weapon = weapon, held = false;
    if (!ranged) return false;
    uint64_t fire_button = 1ull << (ranged != WEAPON_BOW ? BTN_A : weapon_bow_type() ? BTN_R : BTN_X);
    bool fire = (buttons & fire_button) != 0;
    if (!fire) held = false;
    bool cancel = weapon_drawn() != 1 || (buttons & lock_cancel_buttons(ranged));
    if (cancel) held = fire;
    return cancel || held;
}

// A bow's or a bowgun's shot follows the monster only while the left stick is centered.
void mousecam_lock_shot(void) {
    static uint64_t fired;
    static uint32_t previous_hunter, previous_weapon;
    static bool canceled;
    uint32_t h = weapon_hunter();
    int ranged = weapon_ranged(h);
    uint32_t weapon = h ? LD32(h) : 0;
    if (h != previous_hunter || weapon != previous_weapon || !ranged) {
        previous_hunter = h, previous_weapon = weapon, fired = 0, canceled = false;
        if (!ranged) { lock_canceled(h, ranged, 0); return; }
    }
    uint64_t b, now = plat_now_ns();
    int32_t sticks[4];
    input_get(&b, sticks);
    // (a bowgun: A fires; a bow: what draws it, by the game's Bow Controls; A is its melee attack)
    uint64_t fire_button = 1ull << (ranged != WEAPON_BOW ? BTN_A : weapon_bow_type() ? BTN_R : BTN_X);
    bool fire = (b & fire_button) != 0;
    if (!fire) canceled = false;
    uint64_t aim_button = 1ull << (ranged == WEAPON_BOW && weapon_bow_type() ? BTN_X : BTN_R);
    bool cancel = lock_canceled(h, ranged, b) || weapon_aiming(h) || (b & aim_button);
    if (cancel) { fired = 0, canceled = fire; return; }
    if (!touch_lock_on()) { fired = 0; return; }
    if (canceled) return;  // centering the stick cannot undo a dodge, sheathe or manual-aim cancellation
    unsigned action = weapon_action(h);
    if (!weapon_moving(h) && !(fire && action == 0x00010000u)) { fired = 0; return; }
    if (fire) fired = now;
    // A fire press is enough to redirect a locked bowgun shot while standing still. R remains entirely user-controlled.
    bool shooting = fire || (fired && now - fired < SHOT_NS);
    if (!shooting || left_stick_active(sticks)) return;
    float x = LDF(h + PL_PLACE), y = LDF(h + PL_PLACE + 4), z = LDF(h + PL_PLACE + 8), t[3];
    if (!isfinite(x) || !lock_target(x, y, z, t, true)) return;
    float body[3], aim_y = t[1] + LOCK_HEIGHT;  // (no skeleton to read: a fixed height over its feet)
    if (body_center(g_lock_en, t[1], body)) t[0] = body[0], t[2] = body[2], aim_y = body[1];
    float dx = t[0] - x, dz = t[2] - z, dist = sqrtf(dx * dx + dz * dz);
    if (dist < LOCK_NEAR0) return;  // (under it: the bearing swings)
    const float U = 65536.0f / (2.0f * 3.14159265f);
    ST32(h + PL_AIM_YAW, (uint32_t)lrintf(atan2f(dx, dz) * U) & 0xffffu);
    if (ranged != WEAPON_BOW) return;
    float rise = atan2f(aim_y - (y + MUZZLE_Y), dist);  // (radians; up is positive)
    const float UP = 45.0f * 3.14159265f / 180.0f, DOWN = 30.0f * 3.14159265f / 180.0f;
    if (rise > UP) rise = UP;
    if (rise < -DOWN) rise = -DOWN;
    float aim = 100.0f * rise / (rise >= 0.0f ? UP : DOWN);
    STF(h + PL_AIM_PITCH, aim), STF(h + PL_AIM_PITCH2, aim);
    ST16(h + PL_AIM_LEAN, (uint16_t)(int16_t)lrintf(rise * U));
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

// The mouse's and the phone's movement since the last update, as whole units of yaw and pitch (false: neither moved).
static bool take_turn(uint32_t invert, int *iy, int *ip) {
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    g_seen_ns = now;
    pthread_mutex_lock(&g_lock);
    float dx = g_dx, dy = g_dy, myaw = g_mot_yaw, mpitch = g_mot_pitch;
    g_dx = g_dy = g_mot_yaw = g_mot_pitch = 0;
    pthread_mutex_unlock(&g_lock);
    if (!seen || now - seen >= ACTIVE_NS)  // (moved while it didn't run)
        g_rest_yaw = g_rest_pitch = g_trail_dx = g_trail_dy = 0, dx = dy = 0, myaw = mpitch = 0;
    float sm = config_live_mouse_smoothing();  // (smoothing: each update takes the share of what is waiting that 1 - sm says)
    if (sm > 0.0f) {
        g_trail_dx += dx, g_trail_dy += dy;
        dx = g_trail_dx * (1.0f - sm), dy = g_trail_dy * (1.0f - sm);
        g_trail_dx -= dx, g_trail_dy -= dy;
        if (fabsf(g_trail_dx) < 0.01f && fabsf(dx) < 0.01f) g_trail_dx = 0, dx = 0;
        if (fabsf(g_trail_dy) < 0.01f && fabsf(dy) < 0.01f) g_trail_dy = 0, dy = 0;
    } else {
        g_trail_dx = g_trail_dy = 0;
    }
    if (dx == 0 && dy == 0 && myaw == 0.0f && mpitch == 0.0f) return false;
    float s = MOUSECAM_DEG_PER_COUNT * UNITS_PER_DEG * config_live_mouse_sensitivity();
    float yaw = -dx * s, pitch = -dy * s * config_live_mouse_sensitivity_y();  // (right turns the yaw down, up raises the pitch: as the stick)
    if (config_live_mouse_invert_y()) pitch = -pitch;
    if (invert & 1) pitch = -pitch;
    if (invert & 2) yaw = -yaw;
    // (the phone's turning, in radians: [Controls] MotionGain, and its own invert options; the stick's invert is not its)
    float mot = UNITS_PER_RAD * config_motion_gain();
    yaw -= myaw * mot, pitch += mpitch * mot;
    yaw += g_rest_yaw, pitch += g_rest_pitch;
    *iy = (int)yaw, *ip = (int)pitch;
    g_rest_yaw = yaw - (float)*iy, g_rest_pitch = pitch - (float)*ip;
    return true;
}

static void log_turn(const char *what, uint32_t cam, int iy, int ip, uint32_t yaw0, uint32_t pitch0) {
    if (g_log_on < 0) g_log_on = getenv("MHGU_MOUSECAM_LOG") != NULL;
    if (!g_log_on) return;
    LOG("[mousecam] %s turn yaw %+d pitch %+d units; camera yaw %04x -> %04x, pitch %04x -> %04x\n", what, iy, ip, yaw0,
        LD32(cam + QC_YAW_TO) & 0xffffu, pitch0, LD32(cam + QC_PITCH_TO) & 0xffffu);
}

// (control file "camstat": where the hunt camera looks and whether the mouse has it)
void mousecam_status(void) {
    uint32_t cam = g_cam;
    if (!sane(cam)) return (void)LOG("[mousecam] no hunt camera seen yet (active %d)\n", mousecam_active());
    LOG("[mousecam] camera %08x active %d yaw %04x (now %04x) pitch %04x (now %04x) aim-last %d\n", cam, mousecam_active(),
        LD32(cam + QC_YAW_TO) & 0xffffu, LD32(cam + QC_YAW_NOW) & 0xffffu, LD32(cam + QC_PITCH_TO) & 0xffffu,
        LD32(cam + QC_PITCH_NOW) & 0xffffu, g_last_aim);
}

static void camera_stick(Cpu *c) {
    uint32_t cam = c->r[0], invert = c->r[1];
    fnhook_call_original(c, FN_CAMERA_STICK);
    camlog(cam);
    g_cam = cam, g_stick_runs++;
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    if (sane(cam) && seen && now - seen < ACTIVE_NS && touch_lock_active() && !weapon_aiming(weapon_hunter())) {
        float dt = (now - seen) / 1e9f;
        lock_follow(cam, dt > 0.1f ? 0.1f : dt);
    }
    int iy, ip;
    if (!take_turn(invert, &iy, &ip) || !sane(cam)) return;
    g_last_aim = false;
    uint32_t yaw0 = LD32(cam + QC_YAW_TO) & 0xffffu, pitch0 = LD32(cam + QC_PITCH_TO) & 0xffffu;
    if (iy) {
        ST32(cam + QC_YAW_TO, (LD32(cam + QC_YAW_TO) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)iy) & 0xffffu);
    }
    if (ip) {
        int to = (int16_t)LD32(cam + QC_PITCH_TO), next = clamp_pitch(to + ip);
        int now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + next - to);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
        if (next != to + ip) g_rest_pitch = 0;  // (at the limit)
    }
    log_turn("camera", cam, iy, ip, yaw0, pitch0);
}

static float cam_param(Cpu *c, uint32_t cam, uint32_t id) {
    c->r[0] = cam, c->r[1] = id;
    guest_call(c, FN_CAM_PARAM);
    return c->v.f[0];
}

// The hunter's aim pitch moved by ip camera units; the units the camera's pitch really moved (0 at the limits).
static int aim_pitch(Cpu *c, uint32_t cam, uint32_t pl, int ip) {
    int up = (int)(cam_param(c, cam, QC_PARAM_AIM_UP) * UNITS_PER_DEG + 0.5f);
    int down = (uint16_t)(int)(cam_param(c, cam, QC_PARAM_AIM_DOWN) * UNITS_PER_DEG + 0.5f);
    int range = (uint16_t)(up + down);  // (as the aim camera's: its pitch = -down + (aim + 100) * range / 200)
    if (!range) return 0;
    float units_per_aim = (float)range / (2.0f * AIM_PITCH_MAX);
    float was = LDF(pl + PL_AIM_PITCH), aim = was + (float)ip / units_per_aim;
    if (aim > AIM_PITCH_MAX) aim = AIM_PITCH_MAX, g_rest_pitch = 0;
    if (aim < -AIM_PITCH_MAX) aim = -AIM_PITCH_MAX, g_rest_pitch = 0;
    STF(pl + PL_AIM_PITCH, aim);
    STF(pl + PL_AIM_PITCH2, aim);
    return (int)((aim - was) * units_per_aim);
}

// Scoped tracking uses the same body point as a locked shot and the native camera's aim limits.
static bool lock_aim(Cpu *c, uint32_t cam, uint32_t pl, uint32_t mode) {
    if (!config_live_persistent_lock_on() || !touch_lock_on()) return false;
    uint64_t buttons;
    int32_t sticks[4];
    input_get(&buttons, sticks);
    bool canceled = lock_canceled(pl, weapon_ranged(pl), buttons);
    if (canceled || left_stick_active(sticks)) return false;
    float x = LDF(pl + PL_PLACE), y = LDF(pl + PL_PLACE + 4), z = LDF(pl + PL_PLACE + 8), t[3];
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !lock_target(x, y, z, t, true)) return false;
    float body[3];
    if (body_center(g_lock_en, t[1], body)) t[0] = body[0], t[1] = body[1], t[2] = body[2];
    else t[1] += LOCK_HEIGHT;
    float dx = t[0] - x, dz = t[2] - z, dist = sqrtf(dx * dx + dz * dz);
    if (dist < LOCK_NEAR0) return false;
    Cpu params = *c;
    float up = cam_param(&params, cam, QC_PARAM_AIM_UP), down = cam_param(&params, cam, QC_PARAM_AIM_DOWN);
    if (!isfinite(up) || !isfinite(down) || up <= 0 || down <= 0 || up >= 90 || down >= 90) return false;
    int up_units = (int)(up * UNITS_PER_DEG + .5f), down_units = (int)(down * UNITS_PER_DEG + .5f);
    if (!up_units || !down_units) return false;
    int pitch = (int)lrintf(atan2f(t[1] - (y + MUZZLE_Y), dist) * UNITS_PER_RAD);
    if (pitch > up_units) pitch = up_units;
    if (pitch < -down_units) pitch = -down_units;
    if (mode == 1) {
        if (pitch > AIM_LEAN_MAX) pitch = AIM_LEAN_MAX;
        if (pitch < -AIM_LEAN_MAX) pitch = -AIM_LEAN_MAX;
    }
    float aim = (float)(pitch + down_units) * (2.0f * AIM_PITCH_MAX) / (up_units + down_units) - AIM_PITCH_MAX;
    uint32_t yaw = (uint32_t)lrintf(atan2f(dx, dz) * UNITS_PER_RAD) & 0xffffu;
    ST32(pl + PL_AIM_YAW, yaw);
    STF(pl + PL_AIM_PITCH, aim), STF(pl + PL_AIM_PITCH2, aim);
    ST16(pl + PL_AIM_LEAN, (uint16_t)(int16_t)pitch);
    // The native aim camera reads the hunter's angles itself. Its state at +0x968..+0x978 is separate from the
    // hunt camera's +0x9d8..+0x9ec; writing hunt angles here makes close-range movement orbit/tilt toward the body.
    // Leave camera position, distance and both camera states to the game's own update.
    return true;
}

static void aim_turn(Cpu *c) {
    uint32_t pl = c->r[0], mode = c->r[1], cam = g_cam;
    fnhook_call_original(c, FN_AIM_TURN);
    static uint32_t last_runs;
    bool stick_ran = g_stick_runs != last_runs;  // (some options keep the stick on the camera while aiming)
    last_runs = g_stick_runs;
    if (!sane(pl) || !sane(cam) || LD32(cam + QC_PLAYER) != pl || !weapon_ranged(pl)) return;
    // This shared turn helper also runs outside the scope. Use the same weapon/style action query as the native
    // aim camera (0x227870), otherwise persistent tracking steers ordinary movement and tilts the hunt camera.
    Cpu query = *c;
    query.r[0] = pl, query.r[1] = 1, query.r[2] = 0;
    guest_call(&query, FN_AIMING);
    if (!query.r[0]) {
        __atomic_store_n(&g_aim_seen_ns, 0, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&g_aim_hunter, pl, __ATOMIC_RELAXED);
    __atomic_store_n(&g_aim_action, weapon_action(pl), __ATOMIC_RELAXED);
    __atomic_store_n(&g_aim_seen_ns, plat_now_ns(), __ATOMIC_RELEASE);
    int iy, ip;
    bool turned = !stick_ran && take_turn(LD8(LD32(G_OPTIONS) + OPT_AIM_INVERT), &iy, &ip);
    if (lock_aim(c, cam, pl, mode)) {
        g_last_aim = true;
        // Absolute tracking consumes manual input too: nothing queued can jump the aim when the lock is released.
        g_rest_yaw = g_rest_pitch = g_trail_dx = g_trail_dy = 0;
        return;
    }
    if (stick_ran || !turned) return;
    g_last_aim = true;
    uint32_t yaw0 = LD32(pl + PL_AIM_YAW) & 0xffffu, pitch0 = LD32(cam + QC_EYE_PITCH) & 0xffffu;
    if (iy) {
        ST32(pl + PL_AIM_YAW, (LD32(pl + PL_AIM_YAW) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_EYE_YAW, (LD32(cam + QC_EYE_YAW) + (uint32_t)iy) & 0xffffu);
    }
    if (!ip) return;
    if (mode == 1) {
        int lean = (int16_t)LD16(pl + PL_AIM_LEAN) + ip;
        ST16(pl + PL_AIM_LEAN, lean < -AIM_LEAN_MAX ? -AIM_LEAN_MAX : lean > AIM_LEAN_MAX ? AIM_LEAN_MAX : lean);
    } else {
        int moved = aim_pitch(c, cam, pl, ip);
        ST32(cam + QC_EYE_PITCH, (LD32(cam + QC_EYE_PITCH) + (uint32_t)moved) & 0xffffu);
    }
    if (g_log_on == 1) LOG("[mousecam] aim turn yaw %+d pitch %+d units; hunter yaw %04x -> %04x, eye pitch %04x -> %04x (mode %u)\n", iy, ip, yaw0, LD32(pl + PL_AIM_YAW) & 0xffffu, pitch0, LD32(cam + QC_EYE_PITCH) & 0xffffu, mode);
}

void mousecam_init(void) {
    g_log_on = getenv("MHGU_MOUSECAM_LOG") != NULL;
    fnhook_set(FN_CAMERA_STICK, hle_register("mousecam:camera_stick", camera_stick));
    fnhook_set(FN_AIM_TURN, hle_register("mousecam:aim_turn", aim_turn));
}
