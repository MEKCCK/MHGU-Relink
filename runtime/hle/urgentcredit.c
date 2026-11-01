// Urgent quests count for everyone who cleared them, not only for the hunter who posted them (ini [Multiplayer]
// ShareUrgentCredit). Clearing a quest sets its bit in the save's clear flags in 0x588d40, the same for every quest,
// unless this game isn't the poster's (0x5ae3b4: online, and sQuestSelector +0xd788 != +0xd789) and one of:
//   0x5ae75c  an Urgent quest: sQuest +0xb8 bit 12, from bit 0 of cQuestData mFlag (+0x10c)
//   0x5ae768  a Special Permit quest (group 4)
//   0x5a3424  a quest of some requesters (mRequestVillage 5-8, 12) that this save hasn't unlocked
// The first is all that keeps a joiner's Urgent from counting: 0x5ae75c is called from there alone, and nothing else
// reads the bit. What a clear unlocks follows from the flags, so it comes as for the poster. mFlag bit 0 is on 28
// quests (Vaulting Outlaw 206, A Shocking Scoundrel 10307, Dirty Deals 11204...). The save is each player's own, so
// this needs nothing from the host and changes nothing for the others in the quest.
#include "../config.h"
#include "../hle.h"

#define FN_URGENT   0x5ae75cu  // (sQuest) -> an Urgent quest
#define SQ_QUEST_NO 0xe0u      // sQuest: the quest number

static void urgent(Cpu *c) {
    uint32_t q = c->r[0];
    fnhook_call_original(c, FN_URGENT);
    if (!(c->r[0] & 1) || !config_live_share_urgent()) return;
    LOG("[urgent] quest %07u cleared, posted by someone else: it counts here too\n", LD32(q + SQ_QUEST_NO));
    c->r[0] = 0;
}

void urgentcredit_init(void) {
    fnhook_set(FN_URGENT, hle_register("urgentcredit:urgent", urgent));
}
