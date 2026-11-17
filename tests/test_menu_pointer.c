// Real menu-pointer hooks and renderer learning with synthetic guest memory.
// The native cursor fixture models one ordinary step; it does not run the game.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"

static unsigned char guest_memory[0x10000];
static void *test_guest(uint32_t address) {
    assert(address >= 0x08000000u && address - 0x08000000u < sizeof guest_memory);
    return guest_memory + address - 0x08000000u;
}
#undef G2H
#define G2H(a) test_guest((uint32_t)(a))

// The harness runs both production modules on one thread.
#define PTHREAD_MUTEX_INITIALIZER 0
static int pthread_mutex_lock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
static int pthread_mutex_unlock(pthread_mutex_t *mutex) { (void)mutex; return 0; }

static uint64_t clock_ns = 1000000000ull;
volatile uint64_t g_present_count = 1;
float g_render_aspect = 16.0f / 9.0f;
uint64_t plat_now_ns(void) { return clock_ns; }
void config_path(char *path, size_t size) { snprintf(path, size, "menu-pointer-test.ini"); }

#include "../runtime/hle/menuptr.c"

void menulearn_piece(const float *matrix);
void menulearn_piece_final(const float *matrix);
void menulearn_frame(void);

static struct { HostFn fn; uint32_t address; } hooks[9];
static unsigned hook_count, native_calls, helper_calls;
static unsigned mix_calls, combinations;
static uint32_t last_native_bits;
static bool mix_inventory_fixture;
static unsigned mix_materials, mix_output, mix_capacity, mix_failures;
static unsigned mix_popup_exits, mix_list_exits, mix_cursorless_popup_frames, mix_available_popup_returns;
static unsigned mix_release_polls[3];
static uint32_t mix_held, mix_previous_held, mix_active_box;
static bool mix_visibility_fixture, mix_popup_is_visible;
static unsigned mix_visibility_calls, mix_hidden_popup_frames, mix_visible_popup_frames, mix_animation_frames[3];
static uint32_t mix_visibility_registers[16];
static const uint32_t list_cursor = 0x08000100u, other_cursor = 0x08000200u;
static const uint32_t mix_box = 0x08001000u, mix_result_helper = 0x08001200u;
static const uint32_t mix_popup_ui = 0x08001400u;
static const uint32_t mix_helper = 0x38b034u, mix_inner_lr = 0x38b0fcu, mix_prompt_lr = 0x38d1b0u;

static void dispatch_hook(Cpu *cpu, uint32_t address) {
    for (unsigned i = 0; i < hook_count; ++i) {
        if (hooks[i].address != address) continue;
        hooks[i].fn(cpu);
        return;
    }
    assert(!"The actual production hook was not registered");
}

uint32_t hle_register(const char *name, HostFn fn) {
    assert(name && !strncmp(name, "menuptr:", 8) && fn);
    assert(hook_count < sizeof hooks / sizeof *hooks);
    hooks[hook_count].fn = fn;
    return HLE_BASE + 16 * hook_count++;
}
bool fnhook_set(uint32_t address, uint32_t detour) {
    assert(detour >= HLE_BASE && (detour - HLE_BASE) % 16 == 0);
    unsigned index = (detour - HLE_BASE) / 16;
    assert(index < hook_count);
    hooks[index].address = address;
    return true;
}
void guest_call(Cpu *cpu, uint32_t address) {
    if (address == 0x38b6b4u) {
        // The real helper getter reads held menu input, not the pressed bits passed to its update.
        cpu->r[0] = mix_held;
        return;
    }
    assert(!"This menu-pointer case must not need another guest function");
}
void fnhook_call_original(Cpu *cpu, uint32_t address) {
    if (address == FN_MIX_POPUP_VISIBLE) {
        ++mix_visibility_calls;
        memcpy(mix_visibility_registers, cpu->r, sizeof mix_visibility_registers);
        // An independent visibility flag models the setter's observable effect, without executing game code.
        if (cpu->r[0] == mix_popup_ui) mix_popup_is_visible = cpu->r[1] != 0;
        return;
    }
    if (address == FN_MIX_LIST_UPDATE) {
        ++mix_calls;
        uint32_t box = cpu->r[0], bits = cpu->r[1], saved[16];
        memcpy(saved, cpu->r, sizeof saved);
        int state = LD8(box + 0x80);
        if (state == 0 || state == 1) {
            bool selectable = state == 0 || (LD32(box + 4) && LD32(LD32(box + 4) + 0x18) == UINT32_MAX);
            if (selectable) {
                cpu->r[0] = box + (state == 0 ? 0x14 : 0x5c);
                cpu->r[14] = state == 0 ? CALL_MIX_LIST : CALL_MIX_CONFIRM;
                dispatch_hook(cpu, state == 0 ? FN_CURSOR_UPDATE : mix_helper);
            } else ++mix_cursorless_popup_frames;
            if ((bits & 0x2000) && selectable)
                ST8(box + 0x80, state == 0 ? 1 : LD8(box + 0x5c + LIST.row) == 0 ? 2 : 0);
            else if (bits & 0x4000) {
                if (state == 1) ++mix_popup_exits;
                else ++mix_list_exits;
                ST8(box + 0x80, 0);
            }
        } else if (state == 2) {
            if (mix_inventory_fixture) {
                assert(mix_materials && mix_output < mix_capacity);
                --mix_materials;
                if (mix_failures) --mix_failures;
                else { ++mix_output; ST8(LD32(box + 4) + 0x14, 1); }
            }
            ++combinations;
            ST8(box + 0x80, 3);
        } else if (state == 3) {
            ST8(box + 0x80, 4);
            if (mix_inventory_fixture) {
                const float delay = .1f;
                memcpy(G2H(LD32(box + 4) + 0x20), &delay, sizeof delay);
            }
        } else if (state == 4 && LD32(box + 4)) {
            // The real result helper waits on a float countdown, then accepts a new A/B without running a cursor.
            float delay;
            memcpy(&delay, G2H(LD32(box + 4) + 0x20), sizeof delay);
            if (delay > 0) {
                delay -= 1.0f / 30;
                memcpy(G2H(LD32(box + 4) + 0x20), &delay, sizeof delay);
            } else if (bits & 0x6000) {
                if (mix_inventory_fixture) {
                    bool can_combine = mix_materials && mix_output < mix_capacity;
                    ST8(LD32(box + 4) + 0x15, !LD8(LD32(box + 4) + 0x14));
                    ST32(LD32(box + 4) + 0x18, can_combine ? UINT32_MAX : mix_materials ? 1 : 0);
                    ST8(box + 0x5c + LIST.row, can_combine ? 0 : 1);
                    if (can_combine) ++mix_available_popup_returns;
                }
                ST8(box + 0x80, 1);
            } else if (mix_inventory_fixture && (mix_held & 0x2000) && !LD8(LD32(box + 4) + 0x15)) {
                // Native repeat either starts another combination or reports a stopping reason to the popup.
                bool can_combine = mix_materials && mix_output < mix_capacity;
                ST32(LD32(box + 4) + 0x18, can_combine ? UINT32_MAX : mix_materials ? 1 : 0);
                ST8(box + 0x80, can_combine ? 2 : 1);
                if (!can_combine) ST8(box + 0x5c + LIST.row, 1);
            }
        }
        if (mix_visibility_fixture) {
            int current = LD8(box + 0x80);
            Cpu visibility = {0};
            visibility.r[0] = LD32(box + 0x0c);
            visibility.r[1] = current == 1;
            visibility.r[14] = CALL_MIX_POPUP_VISIBLE;
            unsigned before = mix_visibility_calls;
            dispatch_hook(&visibility, FN_MIX_POPUP_VISIBLE);
            assert(mix_visibility_calls == before + 1);
            if (current == 1) {
                if (mix_visibility_registers[1]) ++mix_visible_popup_frames;
                else ++mix_hidden_popup_frames;
            } else if (current >= 2 && current <= 4) {
                ++mix_animation_frames[current - 2];
                assert(mix_visibility_registers[1] == 0);
            }
        }
        memcpy(cpu->r, saved, sizeof saved);
        cpu->r[0] = 0;
        return;
    }
    if (address == mix_helper) {
        ++helper_calls;
        uint32_t saved[16];
        memcpy(saved, cpu->r, sizeof saved);
        // Only the shared helper's nested cursor call is modeled here; its other game work is outside this test.
        cpu->r[14] = mix_inner_lr;
        dispatch_hook(cpu, FN_CURSOR_UPDATE);
        memcpy(cpu->r, saved, sizeof saved);
        return;
    }
    ++native_calls;
    last_native_bits = cpu->r[1];
    unsigned offset, count;
    uint32_t back, on;
    if (address == FN_CURSOR_UPDATE) {
        offset = LIST.row;
        count = LD8(cpu->r[0] + LIST.rows);
        back = BIT_UP; on = BIT_DOWN;
    } else {
        assert(address == FN_CHOICE_UPDATE);
        offset = CHOICE.row;
        count = 2;
        back = cpu->r[2]; on = cpu->r[3];
    }
    assert(count > 0);
    unsigned row = LD8(cpu->r[0] + offset);
    if (cpu->r[1] & back) row = row ? row - 1 : count - 1;
    else if (cpu->r[1] & on) row = (row + 1) % count;
    ST8(cpu->r[0] + offset, row);
    cpu->r[0] = 0;
}

static void invoke(uint32_t address, uint32_t cursor, uint32_t lr, uint32_t bits) {
    Cpu cpu = {0};
    cpu.r[0] = cursor; cpu.r[1] = bits; cpu.r[14] = lr;
    cpu.r[2] = BIT_LEFT; cpu.r[3] = BIT_RIGHT;
    unsigned before = native_calls;
    dispatch_hook(&cpu, address);
    assert(native_calls == before + 1);
}
static void prepare_list(uint32_t cursor, unsigned rows, unsigned pages, unsigned total) {
    memset(test_guest(cursor), 0, 0x40);
    ST8(cursor + LIST.rows, rows);
    ST16(cursor + LIST.pages, pages);
    ST16(cursor + LIST.total, total);
}
static void advance(uint64_t ns) { clock_ns += ns; ++g_present_count; }
static void piece_frame(float x, float y, float z, float half_w, float half_h,
                        float ax, float bx, float ay, float by) {
    float original[16] = {0}, final[16] = {0};
    original[0] = half_w; original[5] = half_h;
    original[12] = x; original[13] = y; original[14] = z;
    memcpy(final, original, sizeof final);
    final[0] *= ax; final[5] *= ay;
    final[12] = ax * x + bx; final[13] = ay * y + by;
    menulearn_piece(original);
    menulearn_piece_final(final);
    menulearn_frame();
}
static void near_value(float actual, float expected) { assert(fabsf(actual - expected) < .01f); }

static void known_first_open(void) {
    MenuGeom geometry;
    prepare_list(list_cursor, 5, 1, 5);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    assert(menuptr_open() && menuptr_rows() == 5 && menuptr_row() == 0);
    assert(menuptr_geometry(&geometry) && geometry.lr == CALL_TITLE_MENU);
    assert(menuptr_hit(640, 185) == 0 && menuptr_hit(640, 269) == 1);
    assert(menuptr_hit(20, 20) == MENUPTR_NONE && !menuptr_take_confirm());

    advance(200000000ull);
    prepare_list(list_cursor, 7, 2, 14);
    invoke(FN_CURSOR_UPDATE, list_cursor, 0x916ad8u, 0);
    assert(menuptr_open() && menuptr_rows() == 7 && menuptr_pages() == 2);
    assert(menuptr_geometry(&geometry) && geometry.lr == 0x916ad8u);
    assert(menuptr_hit(1100, 118) == 0 && menuptr_hit(1100, 154) == 1);

    advance(200000000ull);
    ST8(other_cursor, 0);
    invoke(FN_CHOICE_UPDATE, other_cursor, 0x84bd18u, 0);
    assert(menuptr_open() && menuptr_rows() == 2 && menuptr_row() == 0);
    assert(menuptr_hit(570, 445) == 0 && menuptr_hit(710, 445) == 1);
    assert(!menuptr_take_confirm());
    puts("PASS: shipped title, list and two-way choice hit on their first update without learning or movement");
}

static void selection_and_confirmation(void) {
    prepare_list(list_cursor, 7, 2, 13);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_PAUSE_MENU, 0);
    menuptr_select_at(0, false, 1100, 118);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_PAUSE_MENU, 0);
    assert(menuptr_row() == 0 && !menuptr_take_confirm());
    menuptr_select_at(0, true, 1100, 118);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());

    menuptr_select_at(3, false, 1100, 226);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_PAUSE_MENU, 0);
    assert(LD8(list_cursor + LIST.row) == 3 && menuptr_row() == 3);
    assert(last_native_bits == BIT_DOWN && !menuptr_take_confirm());
    menuptr_select_at(6, true, 1100, 334);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_PAUSE_MENU, 0);
    assert(menuptr_row() == 6 && last_native_bits == BIT_DOWN);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_PAUSE_MENU, 0);
    assert(!menuptr_take_confirm());

    menuptr_select_at(99, true, 1100, 334);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_PAUSE_MENU, 0);
    assert(menuptr_row() == 6 && !menuptr_take_confirm());
    puts("PASS: actual hooks select without confirming, jump by a normal cursor step and confirm only once");
}

static void quiet_cursor_nudge(void) {
    ST8(other_cursor, 0);
    invoke(FN_CHOICE_UPDATE, other_cursor, 0x84bd18u, 0);
    advance(60000000ull);
    piece_frame(570, 445, 97, 1, 1, 1, 0, 1, 0);
    assert(menuptr_open() && menuptr_hit(710, 445) == 1);
    menuptr_select_at(1, false, 710, 445);
    assert(needs_nudge());
    menuptr_nudged();
    invoke(FN_CHOICE_UPDATE, other_cursor, 0x84bd18u, BIT_DOWN);
    assert(LD8(other_cursor) == 1 && menuptr_row() == 1);
    assert(last_native_bits == BIT_RIGHT && !needs_nudge());
    assert(!menuptr_take_confirm());
    menuptr_select_at(1, true, 710, 445);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());
    puts("PASS: renderer-kept quiet choice wakes with its own Right step and never leaks the host Down nudge");
}

static void unknown_foreground(void) {
    prepare_list(list_cursor, 5, 1, 5);
    prepare_list(other_cursor, 2, 1, 2);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    assert(menuptr_hit(640, 185) == 0);
    advance(200000000ull);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    invoke(FN_CURSOR_UPDATE, other_cursor, 0x123456u, 0);
    assert(!menuptr_open() && !menuptr_menu_id());
    assert(menuptr_hit(640, 185) == MENUPTR_NONE);
    menuptr_select_at(0, true, 640, 185);
    assert(!menuptr_take_confirm());
    advance(200000000ull);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    assert(menuptr_open() && menuptr_hit(640, 185) == 0);
    puts("PASS: an unknown foreground cursor blocks clicks on a known background menu until it closes");
}

static void expiry(void) {
    prepare_list(list_cursor, 5, 1, 5);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    uint64_t identity = menuptr_menu_id();
    assert(identity);
    menuptr_select_at(3, true, 640, 437);
    advance(ASK_NS + 1);
    assert(!menuptr_open() && menuptr_hit(640, 437) == MENUPTR_NONE);
    assert(!needs_nudge());
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    assert(menuptr_row() == 0 && !menuptr_take_confirm());
    advance(FRESH_NS + 1);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    assert(menuptr_menu_id() && menuptr_menu_id() != identity);
    assert(!menuptr_take_confirm());
    puts("PASS: expired selection and closed-menu geometry cannot emit a delayed confirm and reopening has a new identity");
}

static void wide_screen_mapping(void) {
    prepare_list(list_cursor, 5, 1, 5);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    g_render_aspect = 20.0f / 9.0f;
    piece_frame(300, 200, 97, 10, 5, .8f, 240, 1, 5);
    float mapping[4];
    assert(menuptr_screen_map(mapping));
    near_value(mapping[0], .8f); near_value(mapping[1], 240);
    near_value(mapping[2], 1); near_value(mapping[3], 5);
    float screen_x = .8f * 640 + 240, screen_y = 185 + 5;
    assert(menuptr_hit((screen_x - mapping[1]) / mapping[0],
                       (screen_y - mapping[3]) / mapping[2]) == 0);
    advance(LIVE_NS + 1);
    assert(!menuptr_screen_map(mapping));
    puts("PASS: actual renderer pieces publish a wide-screen menu offset and stale screen maps expire");
}

static void mix_prompt(void) {
    prepare_list(list_cursor, 8, 23, 184);
    prepare_list(other_cursor, 2, 1, 2);
    invoke(FN_CURSOR_UPDATE, list_cursor, 0x38d468u, 0);
    assert(menuptr_open() && menuptr_hit(1100, 118) == 0);
    advance(200000000ull);
    invoke(FN_CURSOR_UPDATE, list_cursor, 0x38d468u, 0);
    invoke(mix_helper, other_cursor, mix_prompt_lr, 0);
    MenuGeom geometry;
    assert(helper_calls == 1 && menuptr_geometry(&geometry) && geometry.lr == mix_prompt_lr);
    assert(menuptr_rows() == 2 && menuptr_row() == 0);
    near_value(geometry.x, 738); near_value(geometry.y, 408);
    near_value(geometry.w, 114); near_value(geometry.h, 34); near_value(geometry.dy, 36);
    assert(menuptr_hit(795, 425) == 0 && menuptr_hit(795, 461) == 1);
    assert(menuptr_hit(1100, 118) == MENUPTR_NONE && !menuptr_take_confirm());

    g_render_aspect = 20.0f / 9.0f;
    piece_frame(750, 420, 97, 10, 5, .8f, 140, 1, 0);
    float mapping[4];
    assert(menuptr_screen_map(mapping));
    near_value(mapping[0], .8f); near_value(mapping[1], 140);
    near_value(mapping[2], 1); near_value(mapping[3], 0);
    for (int row = 0; row < 2; ++row) {
        float sx = .8f * 795 + 140, sy = 425.0f + 36.0f * row;
        assert(menuptr_hit((sx - mapping[1]) / mapping[0], sy) == row);
    }

    menuptr_select_at(0, false, 795, 425);
    invoke(FN_CURSOR_UPDATE, list_cursor, 0x38d468u, 0);
    invoke(mix_helper, other_cursor, mix_prompt_lr, 0);
    assert(menuptr_row() == 0 && !menuptr_take_confirm());
    menuptr_select_at(0, true, 795, 425);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());

    menuptr_select_at(1, false, 795, 461);
    invoke(mix_helper, other_cursor, mix_prompt_lr, 0);
    assert(menuptr_row() == 1 && LD8(other_cursor + LIST.row) == 1);
    assert(last_native_bits == BIT_DOWN && !menuptr_take_confirm());
    menuptr_select_at(1, true, 795, 461);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());
    assert(LD8(list_cursor + LIST.row) == 0);

    advance(200000000ull);
    invoke(FN_CURSOR_UPDATE, list_cursor, 0x38d468u, 0);
    assert(menuptr_hit(1100, 118) == 0 && !menuptr_take_confirm());
    puts("PASS: the real nested Combine prompt is clickable before movement, owns both rows over its eight-row background and maps on wide screens");
}

static void mix_generic_context(const MenuGeom *baseline) {
    MenuGeom geometry;
    assert(menuptr_geometry(&geometry) && geometry.lr == baseline->lr && geometry.lr != CALL_MIX_CONFIRM);
    near_value(geometry.x, baseline->x); near_value(geometry.y, baseline->y);
    near_value(geometry.w, baseline->w); near_value(geometry.h, baseline->h);
    near_value(geometry.dx, baseline->dx); near_value(geometry.dy, baseline->dy);
    near_value(geometry.cdx, baseline->cdx); near_value(geometry.cdy, baseline->cdy);
    float x = baseline->x + baseline->w * .5f, y = baseline->y + baseline->h * .5f;
    assert(menuptr_hit(x, y) == 0);
    menuptr_touch_select_at(0, true, x, y);
    assert(menuptr_take_confirm() && !menuptr_take_confirm() && !menuptr_mix_buttons(false));
}
static void mix_context_isolation(void) {
    prepare_list(other_cursor, 2, 1, 2);
    MenuGeom baseline;
    assert(menuptr_variants(mix_inner_lr, &baseline, 1) == 1);
    const uint32_t unrelated[] = {0x871d54u, 0x38d1acu};
    for (unsigned i = 0; i < sizeof unrelated / sizeof *unrelated; ++i) {
        invoke(mix_helper, other_cursor, unrelated[i], 0);
        mix_generic_context(&baseline);
        advance(200000000ull);
    }
    invoke(mix_helper, other_cursor, mix_prompt_lr | 1u, 0);
    MenuGeom geometry;
    assert(menuptr_geometry(&geometry) && geometry.lr == mix_prompt_lr);
    assert(menuptr_hit(795, 425) == 0 && !menuptr_take_confirm());
    advance(200000000ull);
    invoke(FN_CURSOR_UPDATE, other_cursor, mix_inner_lr, 0);
    mix_generic_context(&baseline);
    advance(200000000ull);
    invoke(mix_helper, other_cursor, unrelated[0], 0);
    mix_generic_context(&baseline);
    assert(!menuptr_take_confirm());
    puts("PASS: only the exact Combine caller specializes the shared helper; unrelated and restored callers retain their shipped generic geometry and one-shot confirmation without automatic input");
}

static void mix_tick_at(uint32_t box, uint32_t bits) {
    advance(16666667ull);
    Cpu cpu = {0};
    cpu.r[0] = box; cpu.r[1] = bits;
    unsigned before = mix_calls;
    dispatch_hook(&cpu, FN_MIX_LIST_UPDATE);
    assert(mix_calls == before + 1);
}
static void mix_tick(uint32_t bits) { mix_tick_at(mix_box, bits); }
static void mix_delay(float delay) { memcpy(G2H(mix_result_helper + 0x20), &delay, sizeof delay); }
static void prepare_mix(void) {
    prepare_list(mix_box + 0x14, 8, 23, 184);
    prepare_list(mix_box + 0x5c, 2, 1, 2);
    ST32(mix_box + 4, mix_result_helper);
    ST32(mix_box + 0x0c, mix_popup_ui);
    ST8(mix_result_helper + 0x14, 1);
    ST8(mix_result_helper + 0x15, 0);
    ST32(mix_result_helper + 0x18, UINT32_MAX);
}
static void mix_result_repeat(void) {
    prepare_mix();
    mix_tick(0);
    menuptr_select_at(3, false, 1100, 226);
    mix_tick(0);
    assert(LD8(mix_box + 0x14 + LIST.row) == 3);
    menuptr_select_at(3, true, 1100, 226);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());
    mix_tick(0x2000);
    mix_tick(0);
    uint64_t popup_id = menuptr_menu_id();
    const float points[][2] = {{1100, 226}, {600, 300}, {300, 650}};
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        assert(menuptr_hit(795, 425) == 0 && menuptr_hit(795, 461) == 1);
        menuptr_select_at(0, true, 795, 425);
        assert(menuptr_take_confirm() && !menuptr_take_confirm());
        mix_delay(.1f);
        mix_tick(0x2000);
        assert(LD8(mix_box + 0x80) == 2 && !menuptr_open());
        mix_tick(0);
        assert(LD8(mix_box + 0x80) == 3 && !menuptr_open());
        mix_tick(0);
        assert(LD8(mix_box + 0x80) == 4 && !menuptr_open());
        unsigned cursors = native_calls;
        for (int frame = 0; frame < 90; ++frame) mix_tick(0);
        assert(native_calls == cursors && menuptr_open() && menuptr_cursor_ran(OPEN_NS));
        assert(menuptr_hit(1100, 118) == MENUPTR_NONE && menuptr_hit(795, 425) == MENUPTR_NONE);
        float x = points[cycle][0], y = points[cycle][1];
        assert(menuptr_hit(x, y) == 0);
        uint64_t result_id = menuptr_menu_id();
        assert(result_id && result_id != popup_id);
        menuptr_select_at(0, false, x, y);
        assert(!menuptr_take_confirm() && !needs_nudge());
        menuptr_select_at(0, true, x, y);
        menuptr_select_at(0, true, x, y);
        assert(menuptr_take_confirm() && !menuptr_take_confirm());
        menuptr_select_at(0, true, x, y);
        assert(menuptr_take_confirm() && !menuptr_take_confirm()); // A later tap can retry a press merged with held A.
        mix_tick(0x2000);
        mix_tick(0);
        assert(LD8(mix_box + 0x80) == 1 && menuptr_menu_id() != popup_id && menuptr_menu_id() != result_id);
        assert(LD8(mix_box + 0x14 + LIST.row) == 3 && combinations == cycle + 1);
        popup_id = menuptr_menu_id();
    }
    puts("PASS: three complete Combine/result/popup cycles remain clickable after long cursor-free waits and accept single confirms plus explicit retries");
}
static void mix_result_guards(void) {
    prepare_list(list_cursor, 5, 1, 5);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    advance(10000000ull);
    prepare_mix();
    ST8(mix_box + 0x80, 4);
    const float blocked[] = {1.0f, NAN, INFINITY};
    for (unsigned i = 0; i < sizeof blocked / sizeof *blocked; ++i) {
        mix_delay(blocked[i]); mix_tick(0);
        assert(!menuptr_open());
        menuptr_select(0, true);
        assert(!menuptr_take_confirm());
    }
    ST32(mix_box + 4, 0); mix_tick(0);
    assert(!menuptr_open());
    ST32(mix_box + 4, mix_result_helper);
    mix_delay(-.01f); mix_tick(0);
    assert(menuptr_hit(1100, 118) == 0);
    menuptr_select(0, true);
    advance(OPEN_NS + 1);
    assert(!menuptr_open() && !menuptr_take_confirm());
    mix_delay(0); mix_tick(0);
    assert(menuptr_open());
    menuptr_select(0, true);
    mix_tick(0x4000); // A real B closes the result before its queued tap is polled.
    assert(!menuptr_take_confirm());
    mix_tick(0);
    menuptr_select(1, true); // Pending Stop selection must not leak into the next result.
    menuptr_page_turn(1);
    ST8(mix_box + 0x80, 2); mix_tick(0); mix_tick(0);
    assert(!menuptr_take_confirm());
    mix_tick(0x4000); mix_tick(0);
    assert(LD8(mix_box + 0x5c + LIST.row) == 0 && !menuptr_take_confirm());
    ST8(mix_box + 0x80, 4); mix_tick(0);
    advance(120000000ull); mix_tick(0);
    prepare_list(other_cursor, 2, 1, 2);
    invoke(FN_CURSOR_UPDATE, other_cursor, 0x123456u, 0);
    assert(!menuptr_open());
    menuptr_select(0, true);
    assert(!menuptr_take_confirm());
    advance(OPEN_NS + 1); mix_tick(0);
    menuptr_select(0, true);
    advance(120000000ull); mix_tick(0);
    prepare_list(list_cursor, 4, 1, 4);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_OPTIONS, 0);
    assert(!menuptr_take_confirm());
    MenuGeom geometry;
    assert(menuptr_geometry(&geometry) && geometry.lr == CALL_TITLE_OPTIONS);
    advance(OPEN_NS + 1); mix_tick(0);
    g_render_aspect = 20.0f / 9.0f;
    piece_frame(1100, 118, 97, 10, 5, .8f, 140, 1, 0);
    float mapping[4];
    assert(menuptr_screen_map(mapping));
    float sx = .8f * 1100 + 140;
    assert(menuptr_hit((sx - mapping[1]) / mapping[0], 118) == 0);
    ST8(mix_box + 0x80, 7); mix_tick(0);
    assert(!menuptr_open() && !menuptr_take_confirm());
    puts("PASS: result countdown, expiry, cancellation, unrelated foregrounds and wide-screen mapping preserve input isolation");
}

// The game reads held A for native repeat, but only a fresh edge confirms the popup/result.
static void mix_release_host_polls(void) {
    if (!g_mix_result.repeat) return;
    int phase = g_mix_result.phase, index;
    if (phase == MIX_START_RELEASE) index = 0;
    else if (phase == MIX_RESULT_RELEASE) index = 1;
    else if (phase == MIX_EXIT_RELEASE) index = 2;
    else return;
    unsigned calls = mix_calls, attempts = combinations;
    for (int poll = 0; poll < 16; ++poll) assert(!menuptr_mix_buttons(false));
    assert(g_mix_result.phase == phase && mix_calls == calls && combinations == attempts);
    ++mix_release_polls[index];
}
static void mix_auto_tick(uint32_t physical) {
    uint64_t buttons = menuptr_mix_buttons((physical & 0x4000) != 0);
    assert(!(buttons & ~3ull));
    mix_held = physical | (buttons & 1 ? 0x2000 : 0) | (buttons & 2 ? 0x4000 : 0);
    uint32_t pressed = mix_held & ~mix_previous_held;
    mix_previous_held = mix_held;
    mix_tick_at(mix_active_box, pressed);
    assert(!menuptr_take_confirm());
    mix_release_host_polls();
}
static void mix_auto_popup(unsigned materials, unsigned output, unsigned capacity) {
    menuptr_mix_cancel();
    advance(FRESH_NS + OPEN_NS + 1);
    prepare_mix();
    ST8(mix_box + 0x14 + LIST.row, 3);
    ST8(mix_box + 0x80, 1);
    mix_inventory_fixture = true;
    mix_materials = materials; mix_output = output; mix_capacity = capacity; mix_failures = 0;
    mix_popup_exits = mix_list_exits = mix_cursorless_popup_frames = mix_available_popup_returns = 0;
    mix_held = mix_previous_held = 0; mix_active_box = mix_box;
    combinations = 0;
    mix_tick(0);
    assert(menuptr_hit(795, 425) == 0 && menuptr_hit(795, 461) == 1);
    assert(!menuptr_mix_buttons(false) && !menuptr_take_confirm());
}
static void mix_auto_arm(void) {
    menuptr_touch_select_at(0, false, 795, 425);
    assert(!menuptr_mix_buttons(false) && !menuptr_take_confirm());
    menuptr_touch_select_at(0, true, 795, 425);
    assert(!menuptr_take_confirm());
    mix_release_host_polls();
}
static void mix_auto_start(void) {
    mix_auto_arm();
    for (int frame = 0; frame < 6 && LD8(mix_box + 0x80) == 1; ++frame) mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 2);
}
static void mix_auto_result(void) {
    for (int frame = 0; frame < 12 && LD8(mix_box + 0x80) != 4; ++frame) mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 4 && combinations == 1 && menuptr_mix_buttons(false) == 1);
}
static void mix_auto_repeat(void) {
    prepare_list(list_cursor, 5, 1, 5);
    invoke(FN_CURSOR_UPDATE, list_cursor, CALL_TITLE_MENU, 0);
    menuptr_touch_select_at(0, true, 640, 185);
    assert(menuptr_take_confirm() && !menuptr_take_confirm() && !menuptr_mix_buttons(false));

    mix_auto_popup(20, 0, 20);
    // A mouse/ordinary pointer confirm and a physical A never acquire automatic touch ownership.
    menuptr_select_at(0, true, 795, 425);
    assert(menuptr_take_confirm() && !menuptr_take_confirm() && !menuptr_mix_buttons(false));
    mix_auto_tick(0x2000);
    for (int frame = 0; frame < 60; ++frame) mix_auto_tick(0);
    assert(combinations == 1 && LD8(mix_box + 0x80) == 4 && !menuptr_mix_buttons(false));

    mix_auto_popup(20, 0, 20);
    const uint64_t identity = menuptr_menu_id();
    assert(identity);
    mix_auto_start();
    for (int frame = 0; frame < 120 && combinations < 4; ++frame) {
        assert(menuptr_mix_buttons(false) == 1);
        mix_auto_tick(0);
    }
    assert(combinations == 4 && mix_materials == 16 && mix_output == 4);
    assert(mix_available_popup_returns == 0); // Known recipes repeat directly from the result, without reopening the prompt.
    assert(LD8(mix_box + 0x14 + LIST.row) == 3 && menuptr_menu_id() != identity);
    menuptr_mix_cancel();
    unsigned stopped = combinations;
    for (int frame = 0; frame < 60; ++frame) mix_auto_tick(0);
    assert(combinations == stopped && !menuptr_mix_buttons(false));

    mix_auto_popup(20, 0, 20);
    ST8(mix_box + 0x5c + LIST.row, 1); mix_tick(0);
    mix_auto_arm();
    for (int frame = 0; frame < 6 && LD8(mix_box + 0x80) == 1; ++frame) mix_auto_tick(0);
    assert(LD8(mix_box + 0x5c + LIST.row) == 0 && LD8(mix_box + 0x80) == 2);
    menuptr_mix_cancel();

    mix_auto_popup(20, 0, 20);
    mix_held = mix_previous_held = 0x2000; // A was already held before the popup appeared.
    mix_auto_arm();
    for (int frame = 0; frame < 5; ++frame) {
        mix_auto_tick(0x2000);
        assert(LD8(mix_box + 0x80) == 1 && !menuptr_mix_buttons(false) && combinations == 0);
    }
    mix_auto_tick(0); // Only a release actually seen by the native getter may prime a new press.
    for (int frame = 0; frame < 6 && LD8(mix_box + 0x80) == 1; ++frame) mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 2);
    menuptr_mix_cancel();
    puts("PASS: one touchscreen Combine pick owns native held-A across four cycles with no further picks; ordinary pointer/physical confirmation stays manual and explicit cancellation releases it");
}
static void mix_auto_limits(void) {
    const struct { unsigned materials, output, capacity, failures, combinations; bool unknown; } cases[] = {
        {4, 0, 20, 0, 4, false}, {20, 7, 10, 0, 3, false},
        {4, 0, 20, 0, 4, true}, {20, 7, 10, 0, 3, true},
        {20, 0, 3, 2, 5, true}, {2, 0, 20, 2, 2, true},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        mix_auto_popup(cases[i].materials, cases[i].output, cases[i].capacity);
        mix_auto_start();
        mix_failures = cases[i].failures;
        ST8(mix_result_helper + 0x14, !cases[i].unknown);
        ST8(mix_result_helper + 0x15, cases[i].unknown);
        for (int frame = 0; frame < 180; ++frame) mix_auto_tick(0);
        assert(combinations == cases[i].combinations && !menuptr_mix_buttons(false));
        assert(mix_materials == cases[i].materials - cases[i].combinations);
        assert(mix_output == cases[i].output + cases[i].combinations - cases[i].failures);
        assert(!mix_materials || mix_output == mix_capacity);
        assert(LD32(mix_result_helper + 0x18) != UINT32_MAX && LD8(mix_box + 0x80) == 0);
        assert(mix_popup_exits == 1 && mix_list_exits == 0);
        assert(cases[i].unknown ? mix_available_popup_returns > 0 : mix_available_popup_returns == 0);
        assert(mix_cursorless_popup_frames);
        assert(LD8(mix_box + 0x5c + LIST.row) == 1);
        assert(LD8(mix_box + 0x14 + LIST.row) == 3);
        for (int frame = 0; frame < 30; ++frame) mix_auto_tick(0);
        assert(combinations == cases[i].combinations && LD8(mix_box + 0x80) == 0 && !menuptr_take_confirm());
        assert(mix_popup_exits == 1 && mix_list_exits == 0 && !menuptr_mix_buttons(false));
    }
    assert(mix_release_polls[0] && mix_release_polls[1] && mix_release_polls[2]);
    puts("PASS: known/new recipes and synthetic failed attempts continue from one touch until materials run out or output is full, then close only the popup and preserve the selected recipe without restarting");
    puts("PASS: sixteen host-only polls cannot advance start/result/exit release phases without a native game tick observing released A/B");
}
static void mix_auto_cancel(void) {
    for (int mode = 0; mode < 19; ++mode) {
        mix_auto_popup(20, 0, 20);
        if (mode == 2) mix_auto_arm();
        else { mix_auto_start(); mix_auto_result(); }
        switch (mode) {
            case 0: menuptr_mix_cancel(); break;
            case 1: assert(!menuptr_mix_buttons(true)); break;
            case 2: // Touching Stop cancels before any confirmation can be queued.
                menuptr_touch_select_at(1, false, 795, 461);
                mix_auto_tick(0);
                menuptr_touch_select_at(1, true, 795, 461);
                assert(menuptr_take_confirm() && !menuptr_take_confirm());
                mix_auto_tick(0x2000);
                assert(LD8(mix_box + 0x80) == 0); break;
            case 3: // A different recipe cannot inherit the previous touch's hold.
                ST8(mix_box + 0x80, 0); mix_tick(0);
                menuptr_touch_select_at(2, false, 1100, 190); break;
            case 4: menuptr_page_turn(1); break;
            case 5: ST8(mix_box + 0x80, 0); mix_tick(0); break;
            case 6:
                ST8(mix_box + 0x80, 0); mix_tick(0);
                ST8(mix_box + 0x80, 1); mix_tick(0); break;
            case 7: case 8: case 9:
                ST8(mix_box + 0x80, (uint8_t)(mode - 2)); mix_tick(0); break;
            case 10: ST8(mix_box + 0x14 + LIST.row, 4); mix_tick(0); break;
            case 11: ST16(mix_box + 0x14 + LIST.page, 1); mix_tick(0); break;
            case 12:
                ST32(mix_box + 4, mix_result_helper + 0x100);
                ST32(mix_result_helper + 0x118, UINT32_MAX);
                { const float delay = 1; memcpy(G2H(mix_result_helper + 0x120), &delay, sizeof delay); }
                mix_tick(0); break;
            case 13:
                memcpy(G2H(mix_box + 0x400), G2H(mix_box), 0xa0);
                mix_active_box = mix_box + 0x400; mix_tick_at(mix_active_box, 0); break;
            case 14: advance(OPEN_NS + 1); break;
            case 15: case 16:
                advance(120000000ull); mix_tick(0);
                prepare_list(other_cursor, 2, 1, 2);
                invoke(FN_CURSOR_UPDATE, other_cursor, mode == 15 ? 0x123456u : CALL_TITLE_OPTIONS, 0);
                break;
            case 17: mix_auto_tick(0x4000); break;
            case 18:
                mix_auto_tick(0x6000);
                assert(mix_held == 0x6000); break; // Cancelling touch ownership leaves both physical buttons intact.
        }
        assert(!menuptr_mix_buttons(false) && !menuptr_take_confirm());
        unsigned stopped = combinations;
        for (int frame = 0; frame < 30; ++frame) mix_auto_tick(0);
        assert(combinations == stopped && !menuptr_mix_buttons(false));
    }
    puts("PASS: automatic Combine releases for cancellation, Stop, recipe/page/session/helper changes, all terminal menu states, stale/focused-away menus and native B; none resumes without a new touch pick");
}

static void mix_pause_region_matches(const MenuGeom *expected) {
    MenuGeom actual;
    assert(menuptr_mix_pause_region(&actual) && actual.lr == CALL_MIX_CONFIRM);
    float ax0, ay0, ax1, ay1, ex0, ey0, ex1, ey1;
    menuptr_row_rect(&actual, 0, 1, &ax0, &ay0, &ax1, &ay1);
    menuptr_row_rect(expected, 0, 1, &ex0, &ey0, &ex1, &ey1);
    near_value(ax0, ex0); near_value(ay0, ey0); near_value(ax1, ex1); near_value(ay1, ey1);
    near_value(actual.dx, 0); near_value(actual.dy, 0);
    near_value(actual.cdx, 0); near_value(actual.cdy, 0);
    assert(actual.offmask == 0 && actual.cols == 1);
}
static void mix_pause_centre(const MenuGeom *geometry, int row, float *x, float *y) {
    float x0, y0, x1, y1;
    menuptr_row_rect(geometry, row, 1, &x0, &y0, &x1, &y1);
    *x = (x0 + x1) / 2; *y = (y0 + y1) / 2;
}
static void mix_auto_pause(void) {
    const struct { int state, phase; bool ready; } stages[] = {
        {1, MIX_START_RELEASE, false}, {1, MIX_HOLD, false},
        {2, MIX_HOLD, false}, {3, MIX_HOLD, false},
        {4, MIX_HOLD, false}, {4, MIX_HOLD, true},
        {4, MIX_RESULT_RELEASE, false}, {4, MIX_RESULT_PRESS, true},
        {1, MIX_EXIT_RELEASE, false}, {1, MIX_EXIT_PRESS, false},
    };
    for (unsigned mode = 0; mode < sizeof stages / sizeof *stages; ++mode) {
        mix_auto_popup(mode >= 8 ? 1 : 20, 0, 20);
        MenuGeom original;
        assert(menuptr_geometry(&original) && original.lr == CALL_MIX_CONFIRM);
        uint64_t popup_id = menuptr_menu_id();
        if (mode < 2) {
            mix_auto_arm();
            if (mode == 1) mix_auto_tick(0); // Release sampled, but the initial owned A has not been sampled yet.
        } else {
            mix_auto_start();
            if (mode == 3) mix_auto_tick(0);
            else if (mode == 4 || mode == 5) {
                mix_auto_result();
                if (mode == 5) { mix_delay(.001f); mix_auto_tick(0); }
            } else if (mode == 6 || mode == 7) {
                ST8(mix_result_helper + 0x14, 0); ST8(mix_result_helper + 0x15, 1);
                for (int frame = 0; frame < 20 && (LD8(mix_box + 0x80) != 4 ||
                     g_mix_result.phase != stages[mode].phase); ++frame) mix_auto_tick(0);
            } else if (mode >= 8) {
                for (int frame = 0; frame < 30 && g_mix_result.phase != stages[mode].phase; ++frame)
                    mix_auto_tick(0);
            }
        }
        assert(LD8(mix_box + 0x80) == stages[mode].state && g_mix_result.phase == stages[mode].phase);
        assert(g_mix_result.ready == stages[mode].ready);
        if (stages[mode].state >= 2) {
            assert(menuptr_menu_id() != popup_id);
            assert(menuptr_open() == stages[mode].ready);
        }
        mix_pause_region_matches(&original);
        float x, y;
        mix_pause_centre(&original, 0, &x, &y);
        unsigned attempts = combinations, popup_exits = mix_popup_exits, list_exits = mix_list_exits;
        assert(menuptr_mix_pause_at(x, y));
        assert(!menuptr_mix_buttons(false) && !menuptr_take_confirm());
        MenuGeom unused;
        assert(!menuptr_mix_pause_region(&unused) && !menuptr_mix_pause_at(x, y));
        for (int frame = 0; frame < 60; ++frame) mix_auto_tick(0);
        assert(combinations == attempts + (stages[mode].state == 2 ? 1 : 0));
        assert(mix_popup_exits == popup_exits && mix_list_exits == list_exits);
        assert(LD8(mix_box + 0x14 + LIST.row) == 3 && !menuptr_mix_buttons(false));
    }
    puts("PASS: tapping the startup Combine region pauses all ten start/animation/result/terminal-exit phases without A/B or queued confirmation; only an already-started attempt may finish and no automatic popup exit survives");

    mix_auto_popup(20, 0, 20);
    MenuGeom original;
    assert(menuptr_geometry(&original));
    mix_auto_start(); mix_auto_result();
    float x, y;
    mix_pause_centre(&original, 0, &x, &y);
    assert(menuptr_mix_pause_at(x, y));
    for (int frame = 0; frame < 60; ++frame) mix_auto_tick(0);
    assert(combinations == 1 && LD8(mix_box + 0x80) == 4 && menuptr_open());
    menuptr_row_centre(0, &x, &y);
    menuptr_touch_select_at(0, true, x, y);
    assert(menuptr_take_confirm() && !menuptr_take_confirm());
    mix_auto_tick(0x2000); mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 1 && !menuptr_mix_pause_region(&original));
    mix_auto_start();
    for (int frame = 0; frame < 60 && combinations < 3; ++frame) mix_auto_tick(0);
    assert(combinations == 3 && mix_output == 3 && mix_materials == 17);
    menuptr_mix_cancel();
    puts("PASS: a paused result remains manually confirmable; a new Combine tap from the returned popup independently starts another continuous session");
}
static void mix_auto_pause_guards(void) {
    mix_auto_popup(20, 0, 20);
    MenuGeom original, unused;
    assert(menuptr_geometry(&original));
    float x, y;
    mix_pause_centre(&original, 0, &x, &y);
    assert(!menuptr_mix_pause_region(NULL) && !menuptr_mix_pause_region(&unused));
    assert(!menuptr_mix_pause_at(x, y)); // An ordinary popup has no automatic session to pause.
    mix_auto_start();
    float x0, y0, x1, y1, stop_x, stop_y;
    menuptr_row_rect(&original, 0, 1, &x0, &y0, &x1, &y1);
    mix_pause_centre(&original, 1, &stop_x, &stop_y);
    MenuGeom recipe;
    assert(menuptr_variants(CALL_MIX_LIST, &recipe, 1) == 1);
    float recipe_x, recipe_y;
    mix_pause_centre(&recipe, 0, &recipe_x, &recipe_y);
    const float outside[][2] = {
        {stop_x, stop_y}, {x0 - 1, y}, {x1, y}, {x, y0 - 1}, {x, y1},
        {recipe_x, recipe_y}, {NAN, y}, {x, NAN}, {INFINITY, y}, {x, -INFINITY},
    };
    for (unsigned i = 0; i < sizeof outside / sizeof *outside; ++i) {
        assert(!menuptr_mix_pause_at(outside[i][0], outside[i][1]));
        assert(g_mix_result.repeat && menuptr_mix_buttons(false) == 1 && !menuptr_take_confirm());
        mix_pause_region_matches(&original);
    }
    assert(!menuptr_mix_pause_region(NULL) && g_mix_result.repeat);
    menuptr_mix_cancel();

    // Each public pause API independently rejects stale and both known/unknown newer foregrounds.
    for (int foreground = 0; foreground < 3; ++foreground) {
        for (int point_first = 0; point_first < 2; ++point_first) {
            mix_auto_popup(20, 0, 20);
            assert(menuptr_geometry(&original));
            mix_pause_centre(&original, 0, &x, &y);
            mix_auto_start(); mix_auto_result();
            if (!foreground) advance(OPEN_NS + 1);
            else {
                advance(120000000ull); mix_tick(0);
                prepare_list(other_cursor, 2, 1, 2);
                invoke(FN_CURSOR_UPDATE, other_cursor, foreground == 1 ? CALL_TITLE_OPTIONS : 0x123456u, 0);
            }
            if (point_first) {
                assert(!menuptr_mix_pause_at(x, y));
                assert(!menuptr_mix_pause_region(&unused));
            } else {
                assert(!menuptr_mix_pause_region(&unused));
                assert(!menuptr_mix_pause_at(x, y));
            }
            assert(!menuptr_mix_buttons(false) && !menuptr_take_confirm());
            unsigned attempts = combinations;
            for (int frame = 0; frame < 60; ++frame) mix_auto_tick(0);
            assert(combinations == attempts && mix_popup_exits == 0 && mix_list_exits == 0);
        }
    }
    puts("PASS: Stop, recipe-list/outside coordinates, NaN/infinity and absent sessions never pause; both pause APIs independently reject expiry and newer known/unknown foregrounds without leaking input");
}

static MenuGeom mix_pause_picture_rect(const MenuGeom *logical, const float map[4]) {
    float x0, y0, x1, y1;
    menuptr_row_rect(logical, 0, 1, &x0, &y0, &x1, &y1);
    return (MenuGeom){.lr = CALL_MIX_CONFIRM, .x = map[0] * x0 + map[1], .y = map[2] * y0 + map[3],
                      .w = map[0] * (x1 - x0), .h = map[2] * (y1 - y0), .cols = 1};
}
static void mix_auto_pause_mapping(void) {
    const float first[4] = {.6f, 40, .75f, 25}, changed[4] = {1.1f, 180, .9f, 80};
    float current[4], x, y, wrong_x, wrong_y;
    MenuGeom logical;
    mix_auto_popup(20, 0, 20);
    assert(menuptr_geometry(&logical));
    menuptr_set_screen_map(first[0], first[1], first[2], first[3]);
    mix_auto_arm();
    MenuGeom frozen = mix_pause_picture_rect(&logical, first);
    mix_pause_region_matches(&frozen);
    mix_auto_tick(0); mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 2 && !menuptr_open());
    menuptr_set_screen_map(changed[0], changed[1], changed[2], changed[3]);
    MenuGeom moved = mix_pause_picture_rect(&logical, changed);
    mix_pause_centre(&moved, 0, &wrong_x, &wrong_y);
    assert(!menuptr_mix_pause_at(wrong_x, wrong_y));
    mix_pause_centre(&logical, 0, &wrong_x, &wrong_y);
    assert(!menuptr_mix_pause_at(wrong_x, wrong_y));
    for (int frame = 0; frame < 15; ++frame) mix_auto_tick(0);
    assert(!menuptr_screen_map(current) && g_mix_result.repeat);
    mix_pause_region_matches(&frozen);
    mix_pause_centre(&frozen, 0, &x, &y);
    assert(menuptr_mix_pause_at(x, y) && !menuptr_mix_buttons(false) && !menuptr_take_confirm());

    // A later session captures the new transform instead of inheriting its predecessor's pause target.
    mix_auto_popup(20, 0, 20);
    assert(menuptr_geometry(&logical));
    menuptr_set_screen_map(changed[0], changed[1], changed[2], changed[3]);
    mix_auto_arm();
    moved = mix_pause_picture_rect(&logical, changed);
    mix_pause_region_matches(&moved);
    assert(!menuptr_mix_pause_at(x, y));
    mix_pause_centre(&moved, 0, &x, &y);
    assert(menuptr_mix_pause_at(x, y));

    const struct { float aspect, map[4]; } fallback[] = {
        {16.0f / 9.0f, {1, 0, 1, 0}}, {20.0f / 9.0f, {.8f, 128, 1, 0}},
        {4.0f / 3.0f, {1, 0, .75f, 90}}, {0, {1, 0, 1, 0}},
    };
    for (unsigned i = 0; i < sizeof fallback / sizeof *fallback; ++i) {
        g_render_aspect = fallback[i].aspect;
        mix_auto_popup(20, 0, 20);
        assert(!menuptr_screen_map(current) && menuptr_geometry(&logical));
        mix_auto_arm();
        frozen = mix_pause_picture_rect(&logical, fallback[i].map);
        mix_pause_region_matches(&frozen);
        mix_auto_tick(0); mix_auto_tick(0);
        g_render_aspect = i == 2 ? 16.0f / 9.0f : 4.0f / 3.0f;
        mix_pause_region_matches(&frozen);
        x = frozen.x + frozen.w / 2;
        y = i == 2 ? frozen.y + 1 : frozen.y + frozen.h - 1;
        assert(menuptr_mix_pause_at(x, y) && !menuptr_mix_buttons(false));
    }
    g_render_aspect = 16.0f / 9.0f;
    const float invalid[][4] = {{NAN, 0, 1, 0}, {1, INFINITY, 1, 0}, {0, 0, 1, 0},
                                {1, 0, -1, 0}, {1, 0, 1, NAN}};
    for (unsigned i = 0; i <= sizeof invalid / sizeof *invalid; ++i) {
        mix_auto_popup(20, 0, 20);
        if (i < sizeof invalid / sizeof *invalid)
            menuptr_set_screen_map(invalid[i][0], invalid[i][1], invalid[i][2], invalid[i][3]);
        else {
            g_render_aspect = INFINITY;
            menuptr_set_screen_map(1, 0, 1, 0);
        }
        menuptr_touch_select_at(0, false, 795, 425);
        menuptr_touch_select_at(0, true, 795, 425);
        assert(!menuptr_mix_pause_region(&logical) && !menuptr_mix_buttons(false));
        assert(menuptr_take_confirm() && !menuptr_take_confirm());
    }
    g_render_aspect = 16.0f / 9.0f;
    puts("PASS: the pause row freezes its startup picture-space rectangle across non-unit map changes, map expiry, result/menu changes and aspect changes; new sessions recapture maps, no-map wide/narrow/default fallbacks align, and invalid transforms retain ordinary confirmation only");
}

static void mix_visibility_expect(uint32_t ui, uint32_t visible, uint32_t caller, uint32_t expected) {
    Cpu cpu = {0};
    for (unsigned reg = 0; reg < 16; ++reg) cpu.r[reg] = 0x12340000u + reg;
    cpu.r[0] = ui; cpu.r[1] = visible; cpu.r[14] = caller;
    uint32_t registers[16];
    memcpy(registers, cpu.r, sizeof registers);
    registers[1] = expected;
    unsigned before = mix_visibility_calls, updates = mix_calls, attempts = combinations;
    unsigned materials = mix_materials, output = mix_output, failures = mix_failures;
    int state = LD8(mix_box + 0x80);
    dispatch_hook(&cpu, FN_MIX_POPUP_VISIBLE);
    assert(mix_visibility_calls == before + 1);
    assert(!memcmp(mix_visibility_registers, registers, sizeof registers));
    assert(!memcmp(cpu.r, registers, sizeof registers));
    assert(mix_calls == updates && combinations == attempts && LD8(mix_box + 0x80) == state);
    assert(mix_materials == materials && mix_output == output && mix_failures == failures);
    assert(!menuptr_take_confirm());
}
static void mix_auto_visibility(void) {
    mix_visibility_fixture = true;
    mix_auto_popup(6, 0, 6);
    MenuGeom original;
    assert(menuptr_geometry(&original));
    assert(mix_popup_is_visible && mix_visible_popup_frames == 1 && mix_hidden_popup_frames == 0);
    mix_visibility_expect(mix_popup_ui, 1, CALL_MIX_POPUP_VISIBLE, 1);
    mix_auto_arm();
    mix_visibility_expect(mix_popup_ui, 1, CALL_MIX_POPUP_VISIBLE, 0);
    mix_visibility_expect(mix_popup_ui, 1, CALL_MIX_POPUP_VISIBLE | 1u, 0);
    for (int frame = 0; frame < 90 && combinations < 3; ++frame) mix_auto_tick(0);
    assert(combinations == 3 && mix_materials == 3 && mix_output == 3);
    assert(mix_available_popup_returns == 0 && mix_hidden_popup_frames > 0 && mix_visible_popup_frames == 1);
    for (int frame = 0; frame < 10 && LD8(mix_box + 0x80) != 4; ++frame) mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 4 && !mix_popup_is_visible);
    for (unsigned state = 0; state < 3; ++state) assert(mix_animation_frames[state] > 0);
    float x, y;
    mix_pause_centre(&original, 0, &x, &y);
    assert(menuptr_mix_pause_at(x, y));
    for (int frame = 0; frame < 60; ++frame) mix_auto_tick(0);
    assert(combinations == 3 && LD8(mix_box + 0x80) == 4 && menuptr_open());
    mix_auto_tick(0x2000); mix_auto_tick(0);
    assert(LD8(mix_box + 0x80) == 1 && mix_popup_is_visible && mix_visible_popup_frames == 3);
    assert(mix_popup_exits == 0 && mix_list_exits == 0 && mix_materials == 3 && mix_output == 3);

    mix_hidden_popup_frames = mix_visible_popup_frames = 0;
    memset(mix_animation_frames, 0, sizeof mix_animation_frames);
    mix_auto_popup(6, 0, 6);
    ST8(mix_result_helper + 0x14, 0); ST8(mix_result_helper + 0x15, 1); mix_failures = 2;
    mix_auto_start();
    for (int frame = 0; frame < 180; ++frame) mix_auto_tick(0);
    assert(combinations == 6 && mix_materials == 0 && mix_output == 4 && mix_failures == 0);
    assert(mix_available_popup_returns == 3 && mix_hidden_popup_frames >= mix_available_popup_returns);
    assert(mix_visible_popup_frames == 1 && !mix_popup_is_visible);
    assert(LD8(mix_box + 0x80) == 0 && mix_popup_exits == 1 && mix_list_exits == 0);
    for (unsigned state = 0; state < 3; ++state) assert(mix_animation_frames[state] >= combinations);
    mix_visibility_fixture = false;
    puts("PASS: initial/manual Combine prompts remain visible, automatic known/unknown bridges stay hidden from their first frame, all result/animation updates and material/output counts continue, and pausing restores the manually returned prompt");

    for (int mode = 0; mode < 20; ++mode) {
        mix_auto_popup(20, 0, 20);
        if (mode) mix_auto_arm();
        uint32_t ui = mix_popup_ui, visible = 1, caller = CALL_MIX_POPUP_VISIBLE, expected = 1;
        switch (mode) {
            case 0: break; // No automatic touch ownership.
            case 1: expected = 0; break;
            case 2: caller |= 1u; expected = 0; break;
            case 3: ui += 0x200; break;
            case 4: ui = mix_box + 0x0c; break; // The field's address is not its saved UI pointer.
            case 5: caller += 4; break;
            case 6: caller = 0x38db18u; break; // A tail branch is not a call and cannot supply this return LR.
            case 7: ST32(mix_box + 4, mix_result_helper + 0x100); break;
            case 8: ST8(mix_box + 0x14 + LIST.row, 4); break;
            case 9: ST16(mix_box + 0x14 + LIST.page, 1); break;
            case 10: case 11: case 12: case 13:
                ST8(mix_box + 0x80, mode == 10 ? 0 : mode - 9); break;
            case 14: visible = expected = 0; break;
            case 15: visible = expected = 2; break; // Only the literal native show request is suppressed.
            case 16:
                assert(menuptr_geometry(&original));
                mix_pause_centre(&original, 0, &x, &y);
                assert(menuptr_mix_pause_at(x, y)); break;
            case 17: advance(OPEN_NS + 1); break;
            case 18: case 19:
                advance(120000000ull); mix_tick(0);
                prepare_list(other_cursor, 2, 1, 2);
                invoke(FN_CURSOR_UPDATE, other_cursor, mode == 18 ? CALL_TITLE_OPTIONS : 0x123456u, 0);
                break;
        }
        mix_visibility_expect(ui, visible, caller, expected);
        menuptr_mix_cancel();
    }
    puts("PASS: the original visibility setter runs exactly once with every unrelated register preserved; other UIs/field addresses/callers, native hidden/result states, helper/recipe changes, pause, expiry and known/unknown foregrounds pass through unchanged");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    menuptr_init();
    assert(hook_count == 9);
    if (!strcmp(argv[1], "known")) known_first_open();
    else if (!strcmp(argv[1], "select")) selection_and_confirmation();
    else if (!strcmp(argv[1], "nudge")) quiet_cursor_nudge();
    else if (!strcmp(argv[1], "unknown")) unknown_foreground();
    else if (!strcmp(argv[1], "expiry")) expiry();
    else if (!strcmp(argv[1], "mapping")) wide_screen_mapping();
    else if (!strcmp(argv[1], "mix")) mix_prompt();
    else if (!strcmp(argv[1], "context")) mix_context_isolation();
    else if (!strcmp(argv[1], "mix-repeat")) mix_result_repeat();
    else if (!strcmp(argv[1], "mix-guards")) mix_result_guards();
    else if (!strcmp(argv[1], "mix-auto")) mix_auto_repeat();
    else if (!strcmp(argv[1], "mix-auto-limits")) mix_auto_limits();
    else if (!strcmp(argv[1], "mix-auto-cancel")) mix_auto_cancel();
    else if (!strcmp(argv[1], "mix-pause")) mix_auto_pause();
    else if (!strcmp(argv[1], "mix-pause-guards")) mix_auto_pause_guards();
    else if (!strcmp(argv[1], "mix-pause-mapping")) mix_auto_pause_mapping();
    else if (!strcmp(argv[1], "mix-visibility")) mix_auto_visibility();
    else assert(!"Unknown test case");
    return 0;
}
