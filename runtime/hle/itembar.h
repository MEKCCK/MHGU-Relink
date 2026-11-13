// The mouse wheel on the item bar (runtime/hle/itembar.c)
#pragma once

void itembar_init(void);         // hooks the item bar handlers (hunter and Prowler)
bool itembar_active(void);       // its handler ran lately: the wheel is the item bar's, not a menu's
void itembar_scroll(int steps);  // wheel notches: < 0 the item to the left, > 0 to the right (any thread)
// Touch owns one short-lived request, separate from wheel/controller notches. A new direction replaces it.
void itembar_touch_scroll(int direction);
void itembar_touch_cancel(void);  // cancels only touch, including a completed gesture's unconsumed last step
