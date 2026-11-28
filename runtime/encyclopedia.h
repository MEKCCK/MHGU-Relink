// Read-only MHGU encyclopedia data (runtime/encyclopedia.c). The data is embedded in
// the executable (runtime/encyclopedia_data.h); see docs/ENCYCLOPEDIA.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void encyclopedia_init(void);                 // boot: decompress the data compiled into the binary
bool encyclopedia_available(void);
const char *encyclopedia_error(void);         // NULL when loaded, else why not
int encyclopedia_categories(void);
const char *encyclopedia_category_name(int category, int language); // 0 English, 1 Chinese
int encyclopedia_entries(int category);
int encyclopedia_entry_id(int category, int index);
const char *encyclopedia_entry_name(int category, int index, int language);
const char *encyclopedia_entry_detail(int category, int index, int language);
int encyclopedia_entry_icon(int category, int index);       // icon-sheet cell, or 0xFFFF
uint32_t encyclopedia_entry_color(int category, int index); // 0xRRGGBB tint (white: none)
int encyclopedia_icon_sheet_width(void);
int encyclopedia_icon_sheet_height(void);
int encyclopedia_icon_cells(void);
const uint8_t *encyclopedia_icon_sheet(void);                // RGBA32, row-major
const short *encyclopedia_icon_rects(void);                 // [cell*4] x,y,w,h

void encyclopedia_show(void);                 // ask the overlay window to open (any thread)
void encyclopedia_hide(void);
bool encyclopedia_visible(void);
void encyclopedia_draw(float width, float height); // game thread, inside the overlay frame

#ifdef __cplusplus
}
#endif
