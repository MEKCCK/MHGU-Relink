// The in-game MHGU encyclopedia. The compact Chinese/English record, detail and icon
// table is embedded in the executable (runtime/encyclopedia_data.h); the viewer is
// runtime/encyclopedia_ui.cpp.
#include "encyclopedia.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "rt.h"
#include "encyclopedia_data.h"

#define EB_FILE_MAGIC  "MHGUEZ1"  // zlib-compressed payload
#define EB_RAW_MAGIC   "MHGUEB1"  // uncompressed payload
#define EB_VERSION     2u
#define EB_HEADER_SIZE 44u
#define EB_CAT_SIZE    24u
#define EB_REC_SIZE    32u

typedef struct {
    uint32_t id, first, count, name_en, name_zh, reserved;
} Category;
typedef struct {
    uint32_t cat, id, name_en, name_zh, detail_en, detail_zh, icon, color;
} Entry;

static uint8_t *g_raw;
static size_t g_raw_size;
static Category *g_cats;
static Entry *g_entries;
static int g_categories, g_entries_count;
static int g_icon_w, g_icon_h, g_icon_count;
static const uint8_t *g_icon_pixels;
static const short *g_icon_rects;
static const char *g_error;
static int g_open;

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static bool valid_string(uint32_t off) {
    return off < g_raw_size && memchr(g_raw + off, 0, g_raw_size - off) != NULL;
}

static bool parse(uint8_t *raw, size_t size) {
    if (size < EB_HEADER_SIZE || memcmp(raw, EB_RAW_MAGIC, 8) != 0) return false;
    uint32_t version = rd32(raw + 8), cats = rd32(raw + 12), entries = rd32(raw + 16);
    uint32_t pool = rd32(raw + 20);
    uint32_t icon_w = rd32(raw + 24), icon_h = rd32(raw + 28), icon_count = rd32(raw + 32);
    uint32_t icon_off = rd32(raw + 36), rect_off = rd32(raw + 40);
    (void)pool;
    if (version != EB_VERSION || cats == 0 || cats > 64 || entries == 0 || entries > 1000000) return false;
    size_t table = EB_HEADER_SIZE + (size_t)cats * EB_CAT_SIZE + (size_t)entries * EB_REC_SIZE;
    if (table > size) return false;
    if (icon_count) {
        if (!icon_w || !icon_h || icon_w > 4096 || icon_h > 4096 || icon_count > 4096) return false;
        size_t pixels = (size_t)icon_w * icon_h * 4;
        if (icon_off > size || pixels > size - icon_off) return false;
        if (rect_off > size || (size_t)icon_count * 8 > size - rect_off) return false;
    }
    g_cats = (Category *)(raw + EB_HEADER_SIZE);
    g_entries = (Entry *)(raw + EB_HEADER_SIZE + (size_t)cats * EB_CAT_SIZE);
    g_categories = (int)cats;
    g_entries_count = (int)entries;
    g_icon_w = (int)icon_w;
    g_icon_h = (int)icon_h;
    g_icon_count = (int)icon_count;
    g_icon_pixels = icon_count ? raw + icon_off : NULL;
    g_icon_rects = icon_count ? (const short *)(raw + rect_off) : NULL;
    for (uint32_t i = 0; i < cats; i++) {
        Category c = g_cats[i];
        if (c.first > entries || c.count > entries - c.first) return false;
        if (!valid_string(c.name_en) || !valid_string(c.name_zh)) return false;
    }
    for (uint32_t i = 0; i < entries; i++) {
        Entry e = g_entries[i];
        if (e.cat >= cats || !valid_string(e.name_en) || !valid_string(e.name_zh) ||
            !valid_string(e.detail_en) || !valid_string(e.detail_zh))
            return false;
        if (e.icon != 0xFFFFu && icon_count && e.icon >= icon_count) return false;
    }
    if (icon_count) {
        for (uint32_t i = 0; i < icon_count; i++) {
            int x = g_icon_rects[i * 4], y = g_icon_rects[i * 4 + 1];
            int w = g_icon_rects[i * 4 + 2], h = g_icon_rects[i * 4 + 3];
            if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > g_icon_w || y + h > g_icon_h)
                return false;
        }
    }
    return true;
}
static bool load_embedded(void) {
    const uint8_t *file = encyclopedia_blob;
    size_t size = encyclopedia_blob_size;
    uint8_t *raw = NULL;
    if (size >= 16 && memcmp(file, EB_FILE_MAGIC, 8) == 0) {
        uint32_t raw_size = rd32(file + 8), compressed = rd32(file + 12);
        if ((size_t)compressed > size - 16 || (size_t)raw_size > (48u << 20)) return false;
        raw = malloc(raw_size ? raw_size : 1);
        uLongf out = raw_size;
        if (!raw || uncompress(raw, &out, file + 16, compressed) != Z_OK || out != raw_size) {
            free(raw);
            return false;
        }
        file = raw;
        size = raw_size;
    }
    g_raw = (uint8_t *)file;
    g_raw_size = size;
    if (!parse(g_raw, size)) {
        free(raw);
        g_raw = NULL;
        g_raw_size = 0;
        g_cats = NULL;
        g_entries = NULL;
        return false;
    }
    LOG("[encyclopedia] loaded %d categories, %d entries from embedded data\n",
        g_categories, g_entries_count);
    return true;
}

void encyclopedia_init(void) {
    if (load_embedded()) return;
    g_error = "embedded encyclopedia data is invalid";
    LOG("[encyclopedia] %s\n", g_error);
}

bool encyclopedia_available(void) { return g_raw != NULL; }
const char *encyclopedia_error(void) { return g_error; }
int encyclopedia_categories(void) { return g_categories; }

int encyclopedia_entries(int category) {
    if (!g_raw || category < 0 || category >= g_categories) return 0;
    return (int)g_cats[category].count;
}

const char *encyclopedia_category_name(int category, int language) {
    if (!g_raw || category < 0 || category >= g_categories) return "";
    uint32_t off = language == 1 ? g_cats[category].name_zh : g_cats[category].name_en;
    return (const char *)(g_raw + off);
}

static const Entry *entry_at(int category, int index) {
    if (!g_raw || category < 0 || category >= g_categories || index < 0) return NULL;
    Category c = g_cats[category];
    if ((uint32_t)index >= c.count) return NULL;
    return &g_entries[c.first + (uint32_t)index];
}

int encyclopedia_entry_id(int category, int index) {
    const Entry *e = entry_at(category, index);
    return e ? (int)e->id : -1;
}

static const char *entry_string(const Entry *e, uint32_t off) {
    return e ? (const char *)(g_raw + off) : "";
}

const char *encyclopedia_entry_name(int category, int index, int language) {
    const Entry *e = entry_at(category, index);
    if (!e) return "";
    return entry_string(e, language == 1 ? e->name_zh : e->name_en);
}

const char *encyclopedia_entry_detail(int category, int index, int language) {
    const Entry *e = entry_at(category, index);
    if (!e) return "";
    uint32_t off = language == 1 ? e->detail_zh : e->detail_en;
    // Some entries have no Chinese description: fall back to English.
    if (language == 1 && g_raw[off] == 0) off = e->detail_en;
    return entry_string(e, off);
}

int encyclopedia_entry_icon(int category, int index) {
    const Entry *e = entry_at(category, index);
    return e ? (int)e->icon : 0xFFFF;
}

uint32_t encyclopedia_entry_color(int category, int index) {
    const Entry *e = entry_at(category, index);
    return e ? e->color : 0xFFFFFFu;
}

int encyclopedia_icon_sheet_width(void) { return g_icon_w; }
int encyclopedia_icon_sheet_height(void) { return g_icon_h; }
int encyclopedia_icon_cells(void) { return g_icon_count; }
const uint8_t *encyclopedia_icon_sheet(void) { return g_icon_pixels; }
const short *encyclopedia_icon_rects(void) { return g_icon_rects; }


void encyclopedia_show(void) { g_open = 1; }
void encyclopedia_hide(void) { g_open = 0; }
bool encyclopedia_visible(void) { return g_open != 0; }
