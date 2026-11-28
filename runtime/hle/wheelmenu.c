// The + menu's "Configure Shortcut Wheel" row (the game's pause menu, page 3).
//
// The game's pause menu owns two vectors on the menu object: rows (flags + text) and hints. Neither has room for one
// more entry, so on open the row vector is reallocated through the game's own heap (and the hints vector resized by
// its own routine), exactly as MHGU-CraftWheel does for the same 1.4.0 build. Selecting the added row closes the
// pause menu and asks the overlay to open the wheel's item assignments.
//
// The offsets are main-module offsets from 1.4.0; the install checks the native signatures first and does nothing
// when they differ.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "../hle.h"
#include "../rt.h"

#define FN_PLUS_OPEN    0x66feccu   // (menu): builds the list; the row vector is still the native one
#define FN_PLUS_CLOSE   0x66ff3cu
#define FN_PLUS_DESTROY 0x66e3acu
#define FN_PLUS_INPUT   0x66f868u   // (menu): navigation and Confirm; its return 4 closes the menu
#define FN_HEAP_GET     0x7aa47cu   // () -> the game's heap object
#define FN_HINTS_RESIZE 0x625138u   // (hints vector, count, const PlusHint *fill)
#define HEAP_ALLOC_OFF  0x1cu
#define HEAP_FREE_OFF   0x34u
#define OFF_LANGUAGE    0x188ca48u  // GameControl *: +0xa5 the language folder (8 = chS)
#define OFF_PAD         0x188c670u  // the pad object: +0x6bfc the pressed bits
#define OFF_ROW_TABLE_A 0x177aeb8u  // the field / village + menu row-table vtables
#define OFF_ROW_TABLE_B 0x177e424u
#define MENU_ROWS       0xa8u       // { void *data; unsigned size; }
#define MENU_HINTS      0xc0u
#define MENU_PAGE       0xe8u       // u16
#define MENU_CURSOR     0xe5u       // u8 row in the page
#define MENU_PER_PAGE   0xe6u       // u8
#define MENU_MOVE       0xe0u       // u32 (0 up, 1 down, 2 left, 3 right)
#define MENU_COUNT      0xeau       // u16 page count
#define MENU_CURSOR_MAX 0xecu       // u16 rows in the whole list

static uint32_t plus_owner;
static unsigned plus_stock, plus_config_index;
static bool plus_ready;
static atomic_bool plus_pending;
static uint32_t label_addr, empty_addr;

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static uint32_t gcall(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!fn) return 0;
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    uint32_t v = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return v;
}

static unsigned plus_stock_count(uint32_t menu) {
    uint32_t table = LD32(menu);
    if (table == MAIN_BASE + OFF_ROW_TABLE_A) return 13;
    if (table == MAIN_BASE + OFF_ROW_TABLE_B) return 16;
    return 0;
}

static const char *plus_label(void) {
    static const char *const labels[] = {
        "ショートカットの設定",
        "Configure Shortcut Wheel",
        "Configurer les raccourcis",
        "Configurar accesos directos",
        "Kurzmenü einrichten",
        "Configura menu rapido",
        "바로 가기 설정",
        "配置快捷輪盤",
        "配置快捷轮盘",
    };
    uint32_t control = kernel_mapped(MAIN_BASE + OFF_LANGUAGE, 4) ? LD32(MAIN_BASE + OFF_LANGUAGE) : 0;
    unsigned lang = control && kernel_mapped(control, 0xa6) ? LD8(control + 0xa5) : 8;
    return lang < sizeof(labels) / sizeof(labels[0]) ? labels[lang] : labels[1];
}

static void plus_set_rows(uint32_t menu) {
    uint32_t rows = LD32(menu + MENU_ROWS);
    for (unsigned i = plus_stock; i < plus_config_index; ++i) {
        ST32(rows + i * 8u, 0);
        ST32(rows + i * 8u + 4u, empty_addr);
    }
    ST32(rows + plus_config_index * 8u, 1);
    ST32(rows + plus_config_index * 8u + 4u, label_addr);
}

static bool plus_extend(Cpu *c, uint32_t menu, unsigned stock) {
    uint32_t rows = LD32(menu + MENU_ROWS), hints = LD32(menu + MENU_HINTS);
    uint32_t per_page = LD8(menu + MENU_PER_PAGE);
    unsigned count = 2 * per_page + 1;
    if (per_page != (stock == 13 ? 7u : 8u) || !sane(rows) || !sane(hints)) return false;
    if (!kernel_mapped(rows, 8) || !kernel_mapped(hints, 12)) return false;
    uint32_t row_data = LD32(rows), row_size = LD32(rows + 4);
    uint32_t hint_data = LD32(hints), hint_size = LD32(hints + 4);
    uint16_t cursor_count = LD16(menu + MENU_CURSOR_MAX);
    if (!sane(row_data) || !sane(hint_data)) return false;
    if (row_size == stock && hint_size == stock && cursor_count == stock) {
        uint32_t heap = gcall(c, MAIN_BASE + FN_HEAP_GET, MAIN_BASE + 0x191eb18u, 0, 0, 0);
        if (!sane(heap)) return false;
        uint32_t vtable = LD32(heap);
        uint32_t alloc = vtable ? LD32(vtable + HEAP_ALLOC_OFF) : 0;
        uint32_t release = vtable ? LD32(vtable + HEAP_FREE_OFF) : 0;
        if (!sane(alloc) || !sane(release)) return false;
        uint32_t new_rows = gcall(c, alloc, heap, count * 8u, 16, 0);
        if (!sane(new_rows) || !kernel_mapped(new_rows, count * 8u)) return false;
        memcpy(G2H(new_rows), G2H(row_data), stock * 8u);
        uint32_t fill = hle_alloc(12);
        if (!fill) { gcall(c, release, heap, new_rows, 0, 0); return false; }
        memcpy(G2H(fill), G2H(hint_data), 12);
        memset(G2H(fill) + 4, 0xff, 1);
        uint16_t *ids = (uint16_t *)((uint8_t *)G2H(fill) + 6);
        ids[0] = ids[1] = ids[2] = 0xffff;
        uint32_t old_hints = hint_data;
        gcall(c, MAIN_BASE + FN_HINTS_RESIZE, hints, count, fill, 0);
        if (LD32(hints) == old_hints) {
            ST32(hints + 4, stock);
            gcall(c, release, heap, new_rows, 0, 0);
            return false;
        }
        ST32(rows, new_rows);
        ST32(rows + 4, count);
        gcall(c, release, heap, row_data, 0, 0);
    }
    if (LD32(rows + 4) != count || LD32(hints + 4) != count ||
        (cursor_count != stock && cursor_count != count)) return false;
    ST16(menu + MENU_CURSOR_MAX, (uint16_t)count);
    ST16(menu + MENU_COUNT, 3);
    plus_config_index = count - 1;
    plus_set_rows(menu);
    LOG("[wheel] + menu: Configure row added at index %u (stock %u, %u rows)\n", plus_config_index, stock, count);
    return true;
}

static void plus_open(Cpu *c) {
    uint32_t menu = c->r[0];
    fnhook_call_original(c, MAIN_BASE + FN_PLUS_OPEN);
    unsigned stock = sane(menu) ? plus_stock_count(menu) : 0;
    if (!plus_ready || !stock) return;
    if (label_addr) snprintf((char *)G2H(label_addr), 128, "%s", plus_label());
    plus_owner = menu;
    plus_stock = stock;
    plus_config_index = 0;
    if (!plus_extend(c, menu, stock)) {
        LOG("[wheel] + menu: row layout unavailable; the wheel stays configured from the settings menu\n");
        plus_owner = 0;
    }
}

static void plus_close(Cpu *c) {
    if (c->r[0] == plus_owner) plus_owner = 0;
    fnhook_call_original(c, MAIN_BASE + FN_PLUS_CLOSE);
}

static void plus_destroy(Cpu *c) {
    if (c->r[0] == plus_owner) plus_owner = 0;
    fnhook_call_original(c, MAIN_BASE + FN_PLUS_DESTROY);
}

static void plus_input(Cpu *c) {
    uint32_t menu = c->r[0];
    if (!plus_owner || menu != plus_owner || !plus_config_index) {
        fnhook_call_original(c, MAIN_BASE + FN_PLUS_INPUT);
        return;
    }
    uint32_t page = LD16(menu + MENU_PAGE);
    unsigned index = page * LD8(menu + MENU_PER_PAGE) + LD8(menu + MENU_CURSOR);
    if (index == plus_config_index) {
        uint32_t pad = LD32(MAIN_BASE + OFF_PAD);
        uint32_t pressed = pad ? LD32(pad + 0x6bfc) : 0;
        if ((pressed & 0x6000u) == 0x2000u) {  // Confirm (A): the native handler closes with 4
            atomic_store(&plus_pending, true);
            c->r[0] = 4;
            return;
        }
    }
    fnhook_call_original(c, MAIN_BASE + FN_PLUS_INPUT);
    if (menu != plus_owner) return;
    plus_set_rows(menu);
    page = LD16(menu + MENU_PAGE);
    if (page == 1 && page * LD8(menu + MENU_PER_PAGE) + LD8(menu + MENU_CURSOR) >= plus_stock) {
        uint32_t move = LD32(menu + MENU_MOVE);
        ST8(menu + MENU_CURSOR, move == 2 ? 0 : (uint8_t)(plus_stock - LD8(menu + MENU_PER_PAGE) - 1));
    }
}

static bool sign_ok(uint32_t off, uint32_t word) { return LD32(MAIN_BASE + off) == word; }

void wheelmenu_init(void) {
    static const struct { uint32_t offset, word; } signatures[] = {
        {0x66fecc, 0xe92d4830}, {0x66ff3c, 0xe92d4070}, {0x66e3ac, 0xe92d48f0},
        {0x66f868, 0xe92d48f0}, {0x66f8e8, 0xebfb7d6a}, {0x625138, 0xe92d4bf0},
        {0x7aa47c, 0xe59f1010}, {0x677d28, 0xe3a0300d}, {0x78f7a8, 0xe3a03010},
    };
    for (unsigned i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i)
        if (!sign_ok(signatures[i].offset, signatures[i].word)) {
            LOG("[wheel] + menu: native signature mismatch at +%x; the settings menu still configures it\n",
                signatures[i].offset);
            return;
        }
    label_addr = hle_alloc(128);
    empty_addr = hle_alloc(1);
    if (!label_addr || !empty_addr) return;
    plus_ready = fnhook_set(MAIN_BASE + FN_PLUS_OPEN, hle_register("wheelmenu:plus_open", plus_open)) &&
                 fnhook_set(MAIN_BASE + FN_PLUS_CLOSE, hle_register("wheelmenu:plus_close", plus_close)) &&
                 fnhook_set(MAIN_BASE + FN_PLUS_DESTROY, hle_register("wheelmenu:plus_destroy", plus_destroy)) &&
                 fnhook_set(MAIN_BASE + FN_PLUS_INPUT, hle_register("wheelmenu:plus_input", plus_input));
    if (plus_ready) LOG("[wheel] + menu: Configure Shortcut Wheel row ready\n");
    else LOG("[wheel] + menu: hooks unavailable; the settings menu still configures the wheel\n");
}

bool wheelmenu_take_open(void) {
    return atomic_exchange(&plus_pending, false);
}
