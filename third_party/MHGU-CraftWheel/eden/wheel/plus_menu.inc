/* MHGU 1.4.0, FB08F1D20FD1204F. The field and village Start menus share
   a root handler, but own different native lists (13 x 7 and 16 x 8).
   The GUI points to the vector header on the menu, not to its allocation. */
typedef struct { uint32_t flags; const char *text; } PlusRow;
typedef struct {
    void *vtable;
    uint8_t bank, reserved;
    uint16_t file_id, message_id, extra;
} PlusHint;
typedef struct { PlusRow *data; unsigned size; } PlusRows;
typedef struct { PlusHint *data; unsigned size; } PlusHints;
_Static_assert(sizeof(PlusRow) == 8 && sizeof(PlusHint) == 12, "native menu layout");

typedef uintptr_t (*PlusHandler)(uint8_t *);
static PlusHandler plus_open_original, plus_close_original, plus_destroy_original, plus_input_original;
static uint8_t *plus_owner;
static unsigned plus_stock, plus_config_index;
static bool plus_ready, plus_pending;

static unsigned plus_stock_count(const uint8_t *menu) {
    uintptr_t table = *(const uintptr_t *)menu;
    if (table == base + 0x177aeb8) return 13;
    if (table == base + 0x177e424) return 16;
    return 0;
}

/* GameControl uses the language-folder index (jpn, eng, fre, spa,
   ger, ita, kor, chT, chS). Read it again when the native menu refreshes. */
static const char *plus_config_label(void) {
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
    const uint8_t *control = *(const uint8_t *const *)(base + 0x188ca48);
    unsigned lang = control ? control[0xa5] : 8;
    return lang < sizeof(labels) / sizeof(labels[0]) ? labels[lang] : labels[1];
}

static void plus_set_rows(uint8_t *menu) {
    PlusRows *rows = (PlusRows *)(menu + 0xa8);
    for (unsigned i = plus_stock; i < plus_config_index; ++i)
        rows->data[i] = (PlusRow){0, ""};
    rows->data[plus_config_index] = (PlusRow){1, plus_config_label()};
}

static bool plus_extend(uint8_t *menu, unsigned stock) {
    PlusRows *rows = (PlusRows *)(menu + 0xa8);
    PlusHints *hints = (PlusHints *)(menu + 0xc0);
    unsigned per_page = menu[0xe6], count = 2 * per_page + 1;
    uint16_t *cursor_count = (uint16_t *)(menu + 0xec);
    if (per_page != (stock == 13 ? 7u : 8u) || !rows->data || !hints->data)
        return false;
    if (rows->size == stock && hints->size == stock && *cursor_count == stock) {
        /* Same allocator and alignment as both native constructors. The game
           destructor will free these allocations even on a scene change. */
        void *heap = ((void *(*)(void *))(base + 0x7aa47c))((void *)(base + 0x191eb18));
        if (!heap) return false;
        void *(*allocate)(void *, unsigned, unsigned) = method(heap, 0x1c/4);
        void (*release)(void *, void *) = method(heap, 0x34/4);
        PlusRow *new_rows = allocate(heap, count * sizeof(PlusRow), 16);
        if (!new_rows) return false;
        copy(new_rows, rows->data, stock * sizeof(PlusRow));
        PlusHint empty = hints->data[0];
        empty.bank = 0xff;
        empty.file_id = empty.message_id = empty.extra = 0xffff;
        PlusHint *old_hints = hints->data;
        ((void (*)(PlusHints *, unsigned, const PlusHint *))(base + 0x625138))(hints, count, &empty);
        if (hints->data == old_hints) {
            /* The native resize routine sets size even if allocation fails. */
            hints->size = stock;
            release(heap, new_rows);
            return false;
        }
        PlusRow *old_rows = rows->data;
        rows->data = new_rows;
        rows->size = count;
        release(heap, old_rows);
    }
    if (rows->size != count || hints->size != count ||
        (*cursor_count != stock && *cursor_count != count))
        return false;
    *cursor_count = count;
    *(uint16_t *)(menu + 0xea) = 3;
    plus_config_index = count - 1;
    plus_set_rows(menu);
    return true;
}

static uintptr_t plus_open(uint8_t *menu) {
    uintptr_t result = plus_open_original(menu);
    unsigned stock = plus_stock_count(menu);
    if (plus_ready && stock) {
        plus_owner = menu;
        plus_stock = stock;
        plus_config_index = 0;
        if (!plus_extend(menu, stock))
            forge_log(3, "[CraftWheel] native + menu row layout unavailable");
    }
    return result;
}

static uintptr_t plus_close(uint8_t *menu) {
    if (menu == plus_owner) plus_owner = 0;
    return plus_close_original(menu);
}

static uintptr_t plus_destroy(uint8_t *menu) {
    if (menu == plus_owner) plus_owner = 0;
    return plus_destroy_original(menu);
}

static uintptr_t plus_input(uint8_t *menu) {
    if (menu != plus_owner || !plus_config_index)
        return plus_input_original(menu);
    uint16_t page = *(uint16_t *)(menu + 0xe8);
    unsigned index = page * menu[0xe6] + menu[0xe5];
    if (index == plus_config_index) {
        uint8_t *pad = *(uint8_t **)(base + 0x188c670);
        unsigned presses = pad ? *(uint32_t *)(pad + 0x6bfc) : 0;
        /* Use the same logical Confirm/Cancel bits as the native handler.
           Return its close result before it dispatches this custom index. */
        if ((presses & 0x6000) == 0x2000) {
            plus_pending = true;
            return 4;
        }
    }
    uintptr_t result = plus_input_original(menu);
    if (menu != plus_owner) return result;
    /* The original refreshes every descriptor before its cursor update. */
    plus_set_rows(menu);
    page = *(uint16_t *)(menu + 0xe8);
    if (page == 1 && page * menu[0xe6] + menu[0xe5] >= plus_stock) {
        unsigned move = *(uint32_t *)(menu + 0xe0);
        menu[0xe5] = move == 2 ? 0 : (uint8_t)(plus_stock - menu[0xe6] - 1);
    }
    return result;
}

static bool plus_menu_active(void) { return plus_owner != 0; }
static bool plus_menu_take_open(void) {
    bool open = plus_pending;
    plus_pending = false;
    return open;
}

static void plus_menu_install(uint32_t main_base) {
    const struct { uint32_t offset, word; } signatures[] = {
        {0x66fecc,0xe92d4830}, {0x66ff3c,0xe92d4070}, {0x66e3ac,0xe92d48f0},
        {0x66f868,0xe92d48f0}, {0x66f8e8,0xebfb7d6a}, {0x625138,0xe92d4bf0},
        {0x7aa47c,0xe59f1010}, {0x677d28,0xe3a0300d}, {0x78f7a8,0xe3a03010},
    };
    for (unsigned i = 0; i < sizeof(signatures)/sizeof(signatures[0]); ++i) {
        if (*(uint32_t *)(main_base + signatures[i].offset) != signatures[i].word) {
            forge_log(3, "[CraftWheel] native + menu signature mismatch at %08X", signatures[i].offset);
            return;
        }
    }
    forge_hook_create((void *)(main_base + 0x66fecc), plus_open, (void **)&plus_open_original);
    forge_hook_create((void *)(main_base + 0x66ff3c), plus_close, (void **)&plus_close_original);
    forge_hook_create((void *)(main_base + 0x66e3ac), plus_destroy, (void **)&plus_destroy_original);
    forge_hook_create((void *)(main_base + 0x66f868), plus_input, (void **)&plus_input_original);
    plus_ready = plus_open_original && plus_close_original && plus_destroy_original && plus_input_original;
    forge_log(3, "[CraftWheel] native + menu page hook ready=%u", plus_ready);
}
