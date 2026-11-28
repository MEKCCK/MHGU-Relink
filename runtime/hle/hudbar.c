// Independent touch HUD item bar: live pouch snapshot + use-slot command queue.
//
// Threading:
//  - hudbar_frame runs on the game thread, hooked into the unconditional FN_ITEM_BAR handler
//    (runtime/hle/itembar.c), so it can LD16/LD32 guest memory and guest_call safely.
//  - hudbar_use_slot / hudbar_copy are called from the touch (UI) thread: they only touch an atomic
//    request and a mutex-guarded snapshot; they never read guest memory directly.
//
// Anchored on-device (A097b / A103 / A106):
//  * Live manager 19b391f0; entries 4B, code u16 @ +0x1a+4k, count u16 @ +0x1c+4k.
//  * Selected slot u16 @ pl+0x3298; runtime code cache u16 @ pl+0x2520.
//  * Use path: write slot + code cache, then host_input_press("Y", 8) (native item-use button).
//    Bare FN_DO_ACTION 085 is rejected by the vtable+0x718 validator; do not call it directly.
#include <pthread.h>
#include <stdio.h>
#include "hudbar.h"

extern int presence_in_quest(void);

#include "../hle.h"
#include "mousecam.h"

#define PL_ITEM_SLOT 0x3298u    // u16: selected pouch slot (manager entry index)
#define PL_CODE_CACHE 0x2520u   // u16: selected runtime pouch code (idle 0 -> code on use)

// Live pouch manager layout (A097b on-device anchor): entries are 4 bytes each; internal item code u16 @
// +0x1a+4k, held count u16 @ +0x1c+4k. The manager lives in the static/UI segment and its address is fixed
// for this 1.4.0 build. The second, parallel copy of the table sits at +0xe2.
#define MGR_CODE_OFF  0x1au
#define MGR_ENTRY_STRIDE 4u
#define MGR_FALLBACK  0x19b391f0u

// FN_ITEM_NEXT walks the sparse occupied chain from a native slot, dir 0 = right/forward, 1 = left/backward,
// stopping at the 0x28 empty place.
#define FN_ITEM_NEXT  0x4880fcu
// [#107] Eligibility probes shared with itembar.c: is-local / is-busy / vtable+0x230 can-select.
#define FN_PL_IS_LOCAL 0x47bcd0u
#define FN_PL_BUSY     0x47b918u
#define VF_CAN_SELECT  0x230u

static uint32_t hcall(Cpu *c, uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2) {
    uint32_t saved[16];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2;
    guest_call(c, fn);
    uint32_t v = c->r[0];
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    return v;
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_use_queue[32];  // shortcut taps: item ids to use when the snapshot has them
static unsigned g_use_head, g_use_count;

void hudbar_queue_item(uint16_t id) {
    pthread_mutex_lock(&g_mu);
    if (id && g_use_count < sizeof(g_use_queue) / sizeof(*g_use_queue))
        g_use_queue[(g_use_head + g_use_count++) % (sizeof(g_use_queue) / sizeof(*g_use_queue))] = id;
    pthread_mutex_unlock(&g_mu);
}
static HudSlot g_snap[HUDBAR_SLOTS];
static int g_snap_n;
static int index_for_mgr_locked(uint16_t mgr);
static uint16_t g_sel_mgr = 0x28u;   // native selected pouch slot (0x28 = empty "(No Item)" place)
// Self-drawn view anchor: the slot the five-slot window centres on. It follows our own navigation
// (swipe / arrows / jump) but NOT a use: using a side item writes the slot for the validator, yet the
// independent HUD must keep showing the same five slots in place instead of recentring on the used item.
static uint16_t g_view_mgr = 0x28u;
static volatile int g_nav_dirty = 1;  // first frame syncs the view anchor to the native selection
static int g_frame_cnt;      // hudbar_frame invocations (the use lock's reject grace is counted in frames)
static uint32_t g_seen_pl;   // last valid player pointer (debug: detects a rebuilt player object / new session)
// [#107] Per-frame eligibility mirrored from itembar.c (local, not busy, can-select). The host-thread Y
// diversion reads this; a Y press only uses the centre slot when the native bar itself would allow one.
static volatile int g_bar_may;
// [#146] Whether another held item exists to the left / right of the centre (0x28 empty place = that end).

// [#120] Use-action mutex: a use dispatched from either path (tap slot / centre Y) holds the lock until the
// native action itself finishes. The lock follows the hunter's live action code, NOT a fixed action timer:
// at dispatch we snapshot the idle action; we watch for the native to enter the use action (the action code
// changes away from that snapshot, eligibility drops, or the busy probe rises), then hold the lock until the
// action code returns to that snapshot (or a known idle pose). may/busy do not toggle mid-animation on-device,
// so they are not used to time the release. A second use arriving meanwhile is dropped at hudbar_use_slot, so
// one use cannot interrupt the other. The watched count dropping only confirms the action consumed the item;
// it does not time the lock off.
static volatile int g_use_busy;
static uint64_t g_use_dispatch_ns;   // dispatch time (stuck-deadlock guard only; does NOT gate normal release)
static uint16_t g_use_mon_mgr;       // manager entry watched for its count dropping
static uint16_t g_use_count0;        // held count captured at dispatch
static int g_use_consumed;           // 1 once the watched count dropped below its dispatch value
static int g_use_entered;            // 1 once the native action was observed to start
static uint16_t g_use_st0;           // act_st captured at dispatch (idle baseline)
static uint16_t g_use_no0;           // act_no captured at dispatch (idle baseline)
static int g_use_dispatch_frame;     // g_frame_cnt at dispatch (reject grace, frames not wall-time)
#define PL_ACT_ST     0x2506u        // player: current action (u16 act_st @+0x2506, act_no @+0x250a)
#define PL_ACT_NO     0x250au
#define USE_REJECT_GRACE_FRAMES 2u   // frames the native gets to accept the order before a no-show reject
#define USE_STUCK_MS  5000u          // deadlock guard only: releases if an entered action never returns to idle

// [#111] Entering a hunt: recentre on the empty place (0x28) as the centre slot, including the native
// selected slot / code cache. Set on the town->field edge; applied on the first frame with a valid pl, then
// released (or dropped after the max deadline if pl never became valid). Pouch order / items are untouched.
static volatile int g_recenter;
static uint64_t g_recenter_ns;
#define RECENTER_MAX_MS 1500u


// Five visible slots, mirrored straight from the native bar navigation. We do NOT collect our own cycle
// or wrap modulo: each visible position is asked from the native FN_ITEM_NEXT handler the same way the
// native bar draws. Centre = the native selected slot; right = two successive forward steps (dir 0);
// left = two successive backward steps (dir 1). When native reaches the 0x28 empty place that side slot
// stays blank. Index 2 = centre, 3/4 = right, 1/0 = left. -1 = empty place.
static int g_win[5];

// Map a native slot onto the snapshot and place it in a visible window slot (-1 if empty / unknown /
// already shown elsewhere: the five visible slots must not repeat the same item).
static void put_native(int win_slot, uint16_t mgr, bool *used) {
    if (mgr == 0x28u) return;
    int idx = index_for_mgr_locked(mgr);
    if (idx < 0 || g_snap[idx].id == 0 || g_snap[idx].count == 0) return;
    if (used[idx]) return;
    used[idx] = true;
    g_win[win_slot] = idx;
}

static void fill_window(Cpu *c, uint32_t pl) {
    for (int k = 0; k < 5; k++) g_win[k] = -1;
    bool used[HUDBAR_SLOTS] = { false };

    uint16_t sel = g_view_mgr;
    put_native(2, sel, used);  // centre: self-drawn view anchor (0x28 = empty place)

    // Right: two successive native forward neighbours. The 0x28 slot is an in-ring empty placeholder,
    // not an end: when a step lands on it, take one more native NEXT to wrap to the other end (the
    // outer NEXT itself completes the wrap; we do not modulo or collect our own cycle).
    uint32_t cur = sel;
    for (int d = 1; d <= 2; d++) {
        cur = hcall(c, FN_ITEM_NEXT, pl, cur, 0);
        if (cur == 0x28u) cur = hcall(c, FN_ITEM_NEXT, pl, cur, 0);
        if (cur == 0x28u) break;
        put_native(2 + d, (uint16_t)cur, used);
    }
    // Left: two successive native backward neighbours (same placeholder skip/wrap).
    cur = sel;
    for (int d = 1; d <= 2; d++) {
        cur = hcall(c, FN_ITEM_NEXT, pl, cur, 1);
        if (cur == 0x28u) cur = hcall(c, FN_ITEM_NEXT, pl, cur, 1);
        if (cur == 0x28u) break;
        put_native(2 - d, (uint16_t)cur, used);
    }
}

static volatile int g_req_slot = -1;  // touch -> game thread pending use request (-1: none)

void hudbar_notify_nav(void) { g_nav_dirty = 1; }

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

void hudbar_use_slot(int slot) {
    if (slot < 0 || slot >= HUDBAR_SLOTS) return;
    if (g_use_busy) { LOG("[hudbar] use slot %d dropped: previous use still busy\n", slot); return; }
    __atomic_store_n(&g_req_slot, slot, __ATOMIC_RELEASE);
}

int hudbar_copy(HudSlot *out, int max) {
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g_mu);
    int n = g_snap_n < max ? g_snap_n : max;
    for (int i = 0; i < n; i++) out[i] = g_snap[i];
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int index_for_mgr_locked(uint16_t mgr) {
    for (int i = 0; i < g_snap_n; i++)
        if (g_snap[i].mgr_slot == mgr) return i;
    return -1;
}

int hudbar_window(int *out, int max) {
    pthread_mutex_lock(&g_mu);
    int n = 5 < max ? 5 : max;
    for (int i = 0; i < n; i++) out[i] = g_win[i];
    pthread_mutex_unlock(&g_mu);
    return n;
}

extern volatile uint64_t g_town_ns;  // (hle/quickhunt.c: when a town's walking-around update last ran)

// Internal runtime consumable code -> bare item id. On-device anchors:
//   code  9 = id18  Potion 回复药
//   code 24 = id48  Hot Drink 热饮
//   code 26 = id52  Psychoserum 千里眼之药
// A128 test build: hypothesis H1 = id = code * 2 (code = id >> 1). Verify on-device against
// 望远镜 id16 (code8), 冷饮 id46 (code23), 秘药 id56 (code28), 千里眼 id52 (code26).
static uint16_t code_to_id(uint16_t code) {
    return (uint16_t)(code * 2u);
}

// Items that never appear in the field quick-use bar: combo/skill books and the empty placeholder.
// They sit in the pouch manager but cannot be consumed with Y, so our HUD hides them. Extend this
// list as more non-bar items are identified offline from the itm table.
static const uint16_t g_hide_ids[] = {
    2, 4, 6, 8, 10, 12, 14,  // 调和书1..5 / 收纳高手技/力之书
    172, 174,                 // 小木桶 / 大木桶: 放置类, 不进野外快捷栏
    186                       // 未选择道具 placeholder
};
bool hud_item_usable(uint16_t id) {
    if (id == 0) return false;
    for (size_t i = 0; i < sizeof(g_hide_ids) / sizeof(g_hide_ids[0]); i++)
        if (id == g_hide_ids[i]) return false;
    return true;
}

// Read the live pouch into g_snap. Entries are kept POSITIONALLY per manager slot k=0..15: a blank /
// hidden slot stays in place as an id=0 placeholder instead of being compressed out, so consuming an
// item leaves its own slot empty and the neighbours do not slide toward the centre. The render side
// walks this array around the native selected slot, skipping id=0 blanks.
static void fill_snapshot(uint32_t h) {
    (void)h;
    uint32_t obj = MGR_FALLBACK;  // static-segment manager fixed for this build (same anchor as Xth)
    pthread_mutex_lock(&g_mu);
    g_snap_n = 0;
    if (sane(obj) && kernel_mapped(obj + MGR_CODE_OFF, MGR_ENTRY_STRIDE * HUDBAR_SLOTS)) {
        for (int k = 0; k < HUDBAR_SLOTS; k++) {
            uint32_t ent = obj + MGR_CODE_OFF + MGR_ENTRY_STRIDE * (uint32_t)k;
            uint16_t code = LD16(ent);
            uint16_t cnt  = LD16(ent + 2u);
            HudSlot *s = &g_snap[k];
            s->code = code;
            s->count = cnt;
            s->mgr_slot = (uint16_t)k;
            s->id = 0;
            if (code != 0 && cnt != 0) {
                uint16_t mapped = code_to_id(code);
                if (hud_item_usable(mapped)) s->id = mapped;  // non-bar item: blanked here, skipped by the walk
            }
            g_snap_n = k + 1;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void hudbar_refresh(void) { fill_snapshot(0); }

// [#111] Clear every self-drawn bar cache/anchor to the cold-start empty layout. When do_recenter=1 the
// enter-hunt recentre flag is armed so the first valid frame also writes the native selected slot / code
// cache back to the empty place (0x28). Pouch order / items are untouched.
static void bar_reset_empty(int do_recenter, const char *why) {
    pthread_mutex_lock(&g_mu);
    g_use_head = g_use_count = 0;
    g_snap_n = 0;
    for (int k = 0; k < HUDBAR_SLOTS; k++) {
        g_snap[k].id = 0;
        g_snap[k].count = 0;
        g_snap[k].code = 0;
        g_snap[k].mgr_slot = (uint16_t)k;
    }
    for (int k = 0; k < 5; k++) g_win[k] = -1;
    pthread_mutex_unlock(&g_mu);
    g_view_mgr = 0x28u;
    g_sel_mgr = 0x28u;
    g_nav_dirty = 0;
    g_use_busy = 0;
    g_use_consumed = 0;
    g_use_entered = 0;
    g_recenter = do_recenter;
    g_recenter_ns = plat_now_ns();
    __atomic_store_n(&g_req_slot, -1, __ATOMIC_RELEASE);
    LOG("%s\n", why);
}


void hudbar_frame(Cpu *c, uint32_t pl) {
    g_frame_cnt++;
    uint32_t h = weapon_hunter();
    fill_snapshot(h);
    bool native_busy = false;
    if (pl && sane(pl)) {
        g_sel_mgr = LD16(pl + PL_ITEM_SLOT);
        uint32_t vt = LD32(pl);
        uint32_t cs = vt ? LD32(vt + VF_CAN_SELECT) : 0;
        native_busy = hcall(c, FN_PL_BUSY, pl, 0, 0) != 0;
        bool may = hcall(c, FN_PL_IS_LOCAL, pl, 0, 0) == 1 &&
                   !native_busy &&
                   cs && hcall(c, cs, pl, 1, 0) == 1;
        g_bar_may = may ? 1 : 0;
    } else {
        g_bar_may = 0;
    }

    // [#120] While the dispatched action is in flight, track the native action code instead of a timer.
    // Once the watched count drops we note the item was consumed. entered = the native accepted the order
    // (action code moved off the idle snapshot, eligibility dropped, or busy rose). Release when either:
    //  - rejected: within the short frame grace the action never started and eligibility is still up
    //    (the native ignored the use); or
    //  - finished: the action started, and the action code returned to the idle baseline / a known idle
    //    pose. may/native_busy do NOT toggle during the animation on-device, so they are not completion
    //    signals; the stuck guard frees the lock only if an entered action never returns to idle.
    if (g_use_busy) {
        uint64_t nowu = plat_now_ns();
        int cnt_now = -1;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_snap_n; i++) {
            if (g_snap[i].mgr_slot == g_use_mon_mgr) { cnt_now = g_snap[i].count; break; }
        }
        pthread_mutex_unlock(&g_mu);
        if (!g_use_consumed && cnt_now >= 0 && g_use_count0 > 0 && cnt_now < g_use_count0)
            g_use_consumed = 1;
        uint16_t cur_st = (pl && sane(pl)) ? LD16(pl + PL_ACT_ST) : 0;
        uint16_t cur_no = (pl && sane(pl)) ? LD16(pl + PL_ACT_NO) : 0;
        if (!g_use_entered &&
            (!g_bar_may || native_busy || cur_st != g_use_st0 || cur_no != g_use_no0))
            g_use_entered = 1;

        // [#120] Back to idle, NOT may/busy: on-device log2 shows may=1 and native_busy=0 throughout the
        // use animation, so those probes do not toggle and cannot gate completion. Release only after the
        // action code returned to the dispatch snapshot, or to a known idle pose (1:000 standing,
        // 2:016 walking). entered already guarantees the native left idle first, so a mid-action pose
        // matching the idle list cannot release early.
        bool at_baseline = (cur_st == g_use_st0 && cur_no == g_use_no0);
        bool at_idle = (cur_st == 1 && cur_no == 0) || (cur_st == 2 && cur_no == 16);
        bool rejected = !g_use_entered && !g_use_consumed && g_bar_may &&
                        (g_frame_cnt - g_use_dispatch_frame) >= USE_REJECT_GRACE_FRAMES;
        bool finished = g_use_entered && (at_baseline || at_idle);
        bool stuck = g_use_entered && nowu - g_use_dispatch_ns >= USE_STUCK_MS * 1000000ull;
        if (rejected || finished || stuck) g_use_busy = 0;
    }

    // [#111] Quest end / abandon: leaving the hunt back to a town resets the whole self-drawn bar to the
    // cold-start empty layout before the next quest loads. Only our own caches are cleared; the native pouch
    // order / selected-slot logic is untouched. The town->field edge below re-asserts the empty anchor as a
    // second check when departing again.
    {
        uint64_t nowt = plat_now_ns();
        bool in_town = g_town_ns != 0 && nowt - g_town_ns < 1000 * 1000000ull;
        static bool s_was_town = true;
        if (!s_was_town && in_town) {
            char why[128];
            snprintf(why, sizeof(why), "[hudbar] field->town: bar reset to empty (native sel=%u)", g_sel_mgr);
            bar_reset_empty(0, why);
        }
        if (s_was_town && !in_town) {
            char why[128];
            snprintf(why, sizeof(why), "[hudbar] town->field: reset view anchor %u -> 0x28 (native sel=%u)",
                     g_view_mgr, g_sel_mgr);
            bar_reset_empty(1, why);
        }
        // [#111] Independent session-rebuild signal: the player object pointer changes when a quest loads /
        // reloads even if the return-to-town screen never refreshes g_town_ns (so the town edge above stays
        // stuck in field state and never re-arms the recentre). On a changed pointer, reset the self-drawn
        // bar and arm the recentre so the new session opens with the empty place centred.
        if (pl && sane(pl)) {
            if (g_seen_pl && pl != g_seen_pl) {
                char why[160];
                snprintf(why, sizeof(why), "[hudbar] pl changed %08x -> %08x (frame %d): bar reset + recentre",
                         g_seen_pl, pl, g_frame_cnt);
                bar_reset_empty(1, why);
            }
            g_seen_pl = pl;
        }
        s_was_town = in_town;
    }

    // [#111] Apply the enter-hunt recentre: on the first valid frame write the native selected slot / code
    // cache back to the empty place (0x28) and keep the self-drawn anchor there, so the blank slot opens in
    // the middle. Released after applying; a short max deadline drops it if pl never becomes valid.
    if (g_recenter) {
        if (pl && sane(pl)) {
            ST16(pl + PL_ITEM_SLOT, 0x28u);
            ST16(pl + PL_CODE_CACHE, 0);
            g_view_mgr = 0x28u;
            g_sel_mgr = 0x28u;
            g_nav_dirty = 0;
            g_recenter = 0;
            LOG("[hudbar] recentred to empty centre 0x28 (frame %d)\n", g_frame_cnt);
        } else if (plat_now_ns() - g_recenter_ns >= RECENTER_MAX_MS * 1000000ull) {
            g_recenter = 0;
        }
    }

    // Only our own navigation (swipe / arrows / jump) moves the self-drawn window; a use writes the
    // selected slot for the validator but must not recentre the view, so the anchor holds its old value.
    if (g_nav_dirty) { g_view_mgr = g_sel_mgr; g_nav_dirty = 0; }

    // Walk the native FN_ITEM_NEXT chain both ways from the selected slot and cache the five visible
    // positions (centre + two each side), mapped onto the snapshot. The HUD window reads these directly.
    if (pl && sane(pl)) {
        pthread_mutex_lock(&g_mu);
        fill_window(c, pl);
        pthread_mutex_unlock(&g_mu);
    }

    if (h != pl || !sane(pl) || !presence_in_quest()) return;

    int req = __atomic_exchange_n(&g_req_slot, -1, __ATOMIC_ACQ_REL);

    // Resolve the requested HUD slot against the live snapshot (under lock), then point the native
    // selected-slot / code cache at it and let the native input path use it.
    // On-device anchor (A106): bare DO_ACTION 085 is rejected by the vtable+0x718 validator; the
    // working path writes slot@0x3298 + code@0x2520, then synthesizes a native Y press (the item-use
    // face button) so the 0x4ea state machine does eligibility / pre-writes / dispatch itself.
    HudSlot target;
    pthread_mutex_lock(&g_mu);
    // A shortcut tap names an item; use it only when the native state machine is idle, as the bar's own taps.
    if (req < 0 && !g_use_busy && g_bar_may && g_use_count) {
        unsigned st = LD16(pl + PL_ACT_ST), no = LD16(pl + PL_ACT_NO);
        if ((st == 1 && no == 0) || (st == 2 && no == 16)) {
            uint16_t id = g_use_queue[g_use_head];
            g_use_head = (g_use_head + 1) % (sizeof(g_use_queue) / sizeof(*g_use_queue));
            --g_use_count;
            for (int i = 0; i < g_snap_n; ++i)
                if (g_snap[i].id == id && g_snap[i].count) { req = i; break; }
        }
    }
    if (req < 0 || req >= g_snap_n || g_snap[req].count == 0) { pthread_mutex_unlock(&g_mu); return; }
    target = g_snap[req];
    pthread_mutex_unlock(&g_mu);

    ST16(pl + PL_ITEM_SLOT, target.mgr_slot);
    ST16(pl + PL_CODE_CACHE, target.code);
    host_input_press("Y", 8);
    // [#119] Mark the action in flight; the lock clears in hudbar_frame once the native action returns to
    // idle (eligible + not busy), driven by live action state rather than a fixed timer.
    g_use_busy = 1;
    g_use_dispatch_ns = plat_now_ns();
    g_use_dispatch_frame = g_frame_cnt;
    g_use_mon_mgr = target.mgr_slot;
    g_use_count0 = target.count;
    g_use_consumed = 0;
    g_use_entered = 0;
    g_use_st0 = LD16(pl + PL_ACT_ST);
    g_use_no0 = LD16(pl + PL_ACT_NO);
}

void hudbar_init(void) {
    // Hooks are reused from itembar.c (unconditional FN_ITEM_BAR handler); nothing to register here yet.
}
