// Mouse look on the hunt camera's own angles (runtime/hle/mousecam.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { WEAPON_BOW = 1, WEAPON_BOWGUN };  // (runtime/hle/weapon.c)
uint32_t weapon_hunter(void);
int weapon_type(void);  // game's weapon id, 0..4 / 6..15; -1 when unknown
int weapon_ranged(uint32_t hunter);
bool weapon_aiming(uint32_t hunter);
bool weapon_moving(uint32_t hunter);
unsigned weapon_action(uint32_t hunter);  // st << 16 | no
int weapon_bow_type(void);
void mousecam_lock_shot(void);         // (each update of the hunter, hle/itembar.c)

void mousecam_init(void);              // hooks the quest camera's stick handler
bool mousecam_active(void);            // the hunt camera takes the mouse now (its stick handler ran just now)
void mousecam_status(void);            // log where the hunt camera looks (control command "camstat")
void mousecam_add(float dx, float dy);  // counts the mouse moved (x right, y down; any thread)
// The phone turned by (yaw, pitch) radians: yaw to the right, pitch looking up (runtime/host_input.c's motion sensor;
// MotionGain and the camera's units are applied in take_turn), and forgotten again (a mode turned off meanwhile)
void mousecam_motion_add(float yaw, float pitch);  // any thread
void mousecam_motion_reset(void);                  // any thread
// The lock-on's next large monster (runtime/touch.cpp's target control): the hunt camera takes it on its next update
void mousecam_target_cycle(void);                  // any thread
