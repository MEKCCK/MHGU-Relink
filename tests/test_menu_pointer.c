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

static struct { HostFn fn; uint32_t address; } hooks[8];
static unsigned hook_count, native_calls, helper_calls;
static unsigned mix_calls, combinations;
static uint32_t last_native_bits;
static const uint32_t list_cursor = 0x08000100u, other_cursor = 0x08000200u;
static const uint32_t mix_box = 0x08001000u, mix_result_helper = 0x08001200u;
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
    (void)cpu; (void)address;
    assert(!"This menu-pointer case must not need another guest function");
}
void fnhook_call_original(Cpu *cpu, uint32_t address) {
    if (address == FN_MIX_LIST_UPDATE) {
        ++mix_calls;
        uint32_t box = cpu->r[0], bits = cpu->r[1], saved[16];
        memcpy(saved, cpu->r, sizeof saved);
        int state = LD8(box + 0x80);
        if (state == 0 || state == 1) {
            cpu->r[0] = box + (state == 0 ? 0x14 : 0x5c);
            cpu->r[14] = state == 0 ? CALL_MIX_LIST : CALL_MIX_CONFIRM;
            dispatch_hook(cpu, state == 0 ? FN_CURSOR_UPDATE : mix_helper);
            if (bits & 0x2000) ST8(box + 0x80, state == 0 ? 1 : LD8(box + 0x5c + LIST.row) == 0 ? 2 : 0);
        } else if (state == 2) {
            ++combinations;
            ST8(box + 0x80, 3);
        } else if (state == 3) {
            ST8(box + 0x80, 4);
        } else if (state == 4 && LD32(box + 4)) {
            // The real result helper waits on a float countdown, then accepts a new A/B without running a cursor.
            float delay;
            memcpy(&delay, G2H(LD32(box + 4) + 0x20), sizeof delay);
            if (delay > 0) {
                delay -= 1.0f / 30;
                memcpy(G2H(LD32(box + 4) + 0x20), &delay, sizeof delay);
            } else if (bits & 0x6000) {
                ST8(box + 0x80, 1);
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
    assert(menuptr_needs_nudge());
    menuptr_nudged();
    invoke(FN_CHOICE_UPDATE, other_cursor, 0x84bd18u, BIT_DOWN);
    assert(LD8(other_cursor) == 1 && menuptr_row() == 1);
    assert(last_native_bits == BIT_RIGHT && !menuptr_needs_nudge());
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
    assert(!menuptr_needs_nudge());
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

static void mix_context_isolation(void) {
    prepare_list(other_cursor, 2, 1, 2);
    const uint32_t unrelated[] = {0x871d54u, 0x38d1acu};
    for (unsigned i = 0; i < sizeof unrelated / sizeof *unrelated; ++i) {
        invoke(mix_helper, other_cursor, unrelated[i], 0);
        assert(!menuptr_open() && menuptr_hit(795, 425) == MENUPTR_NONE);
        advance(200000000ull);
    }
    invoke(mix_helper, other_cursor, mix_prompt_lr | 1u, 0);
    MenuGeom geometry;
    assert(menuptr_geometry(&geometry) && geometry.lr == mix_prompt_lr);
    assert(menuptr_hit(795, 425) == 0 && !menuptr_take_confirm());
    advance(200000000ull);
    invoke(FN_CURSOR_UPDATE, other_cursor, mix_inner_lr, 0);
    assert(!menuptr_open() && !menuptr_geometry(&geometry));
    advance(200000000ull);
    invoke(mix_helper, other_cursor, unrelated[0], 0);
    assert(!menuptr_open() && menuptr_hit(795, 425) == MENUPTR_NONE);
    assert(!menuptr_take_confirm());
    puts("PASS: only the Combine prompt return address specializes the shared helper and its context is restored after returning");
}

static void mix_tick(uint32_t bits) {
    advance(16666667ull);
    Cpu cpu = {0};
    cpu.r[0] = mix_box; cpu.r[1] = bits;
    unsigned before = mix_calls;
    dispatch_hook(&cpu, FN_MIX_LIST_UPDATE);
    assert(mix_calls == before + 1);
}
static void mix_delay(float delay) { memcpy(G2H(mix_result_helper + 0x20), &delay, sizeof delay); }
static void prepare_mix(void) {
    prepare_list(mix_box + 0x14, 8, 23, 184);
    prepare_list(mix_box + 0x5c, 2, 1, 2);
    ST32(mix_box + 4, mix_result_helper);
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
        assert(!menuptr_take_confirm() && !menuptr_needs_nudge());
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

int main(int argc, char **argv) {
    assert(argc == 2);
    menuptr_init();
    assert(hook_count == 8);
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
    else assert(!"Unknown test case");
    return 0;
}
