// The six-slot item bar's live pouch snapshot and use queue (runtime/hle/hudbar.c).
//
// hudbar_frame runs on the game thread, from the unconditional FN_ITEM_BAR hook (runtime/hle/itembar.c), so it can
// read guest memory and call the game's navigation functions. hudbar_copy / hudbar_window / hudbar_use_slot /
// hudbar_queue_item run on the touch thread and only touch the mutex-guarded snapshot or an atomic request.
//
// Anchors: live manager 0x19b391f0; entries 4 bytes, internal item code u16 @ +0x1a+4k, count u16 @ +0x1c+4k.
// Selected slot u16 @ pl+0x3298; runtime code cache u16 @ pl+0x2520. Use: write slot + code cache, then synthesize
// the native Y press (the game's own item-use state machine validates and dispatches it).
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct Cpu;

typedef struct {
    uint16_t id;       // bare item id (0: empty slot)
    uint16_t count;    // held count
    uint16_t code;     // runtime pouch code (pre code->id mapping)
    uint16_t mgr_slot; // manager entry index this snapshot entry came from
} HudSlot;

#define HUDBAR_SLOTS 50  // the manager's entry table (the game caps it at 0x32)

void hudbar_init(void);                            // main thread, after itembar_init
void hudbar_frame(struct Cpu *c, uint32_t pl);    // game thread, per item-bar update
void hudbar_use_slot(int slot);                    // touch thread: request using a snapshot slot
void hudbar_queue_item(uint16_t id);               // touch thread: use this item when the pouch has it
void hudbar_notify_nav(void);                      // our own navigation moved the selection: sync the view anchor
int  hudbar_copy(HudSlot *out, int max);           // touch thread: snapshot copy
void hudbar_refresh(void);                         // game thread: reread the pouch (the + menu's wheel page)
int  hudbar_window(int *out, int max);             // five visible snapshot indices [L2,L1,C,R1,R2], -1 empty
