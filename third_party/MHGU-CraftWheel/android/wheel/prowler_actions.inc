/* Generated from nativeNX resident ARC support-action records and localized GMDs. */
static const struct {
    const char *zh, *en;
    unsigned char icon, color;
} prowler_action_art[] = {
    {"未选择支援行动", "(No Move)", 46, 0},
    {"药草笛之技", "Herb Horn", 34, 9},
    {"回复笛之技", "Health Horn", 34, 2},
    {"真･回复笛之技", "True Health Horn", 34, 13},
    {"解毒･除臭笛之技", "Detox Horn", 34, 3},
    {"应援笛之技", "Cheer Horn", 34, 4},
    {"鬼人笛之技", "Demon Horn", 34, 1},
    {"硬化笛之技", "Armor Horn", 34, 7},
    {"超音波笛之技", "Ultrasonic Horn", 34, 0},
    {"迷你桶爆弹之技", "Mini Barrel Bombay", 26, 11},
    {"小桶爆弹之技", "Barrel Bombay", 26, 4},
    {"大桶爆弹之技", "Big Barrel Bombay", 26, 7},
    {"大桶爆弹G之技", "Mega Barrel Bombay", 26, 1},
    {"特大桶爆弹之技", "Giga Barrel Bombay", 26, 5},
    {"木桶飞弹之技", "Bounce Bombay", 26, 3},
    {"闪光爆弹之技", "Flash Bombay", 30, 4},
    {"设置型爆弹之技", "Anti-Monster Mine", 24, 7},
    {"设置型爆弹G之技", "Anti-Monster Mine+", 24, 1},
    {"地洞之技", "Pitfall Purr-ison", 24, 2},
    {"麻痹陷阱之技", "Shock Purr-ison", 24, 5},
    {"毒地洞之技", "Poison Purr-ison", 24, 8},
    {"弹簧床之技", "Trampoliner", 80, 8},
    {"猫式活力壶之技", "Vase of Vitality", 27, 12},
    {"回复赠礼之技", "Parting Gift", 29, 2},
    {"紧急撤退之技", "Emergency Retreat", 81, 3},
    {"巨大回力标之技", "Big Boomerangs", 35, 7},
    {"贯穿回力标之技", "Piercing Boomerangs", 35, 3},
    {"隐蔽防御之技", "Camouflage", 25, 11},
    {"武器强化之技", "Weapon Upgrade", 76, 1},
    {"愤怒之技", "Furr-ious", 85, 1},
    {"挑衅之技", "Taunt", 85, 3},
    {"随从鼓舞之技", "Palico Rally", 85, 7},
    {"异臭球之技", "Dung Bombay", 16, 11},
    {"应援舞蹈之技", "Go, Fight, Win", 88, 4},
    {"治･翻滚之技", "Soothing Roll", 88, 2},
    {"爆･翻滚之技", "Explosive Roll", 88, 11},
    {"抢夺之技", "Pilfer", 45, 13},
    {"远程抢夺之技", "Plunderang", 45, 5},
    {"猫突击之技", "Felyne Comet", 76, 4},
    {"猫爪乱舞之技", "Claw Dance", 10, 4},
    {"踏地冲击之技", "Sumo Stomp", 28, 0},
    {"毛栗大炮之技", "Chestnut Cannon", 15, 10},
    {"发射回旋电虫之技", "Shock Tripper", 6, 4},
    {"来挖这边喵之技", "Excavator", 28, 8},
    {"巨型回力标之技 ", "Mega Boomerang", 35, 1},
    {"猫式火龙车之技", "Rath-of-Meow", 40, 1},
    {"野兽变身之技", "Release the Beast", 10, 14},
    {"强化咆哮之技", "Rousing Roar", 28, 6},
    {"猫式回复壶之技", "Furlask of Healing", 25, 2},
    {"回归球之技", "Far-cat-ster", 27, 2},
    {"钻地突进之技", "Burrowing Blow", 76, 11},
    {"设置炮台之技", "Stunt Cannon", 40, 12},
    {"铁･喵球滚滚之技", "Iron Transpurrter", 12, 10},
    {"爆･喵球滚滚之技", "Explosive Transpurrter", 12, 11},
    {"猫式广域笛之技", "Widewhiskers Horn", 23, 7},
    {"SP笛之技", "SP Horn", 34, 4},
    {"禁忌橡子之技", "Furbidden Acorn", 8, 8},
    {"野兽解除之技", "Cage the Beast", 10, 3},
};

static const char *prowler_action_name(unsigned id, int use_english){
    if(!id || id >= sizeof(prowler_action_art)/sizeof(prowler_action_art[0]))return "";
    return use_english ? prowler_action_art[id].en : prowler_action_art[id].zh;
}

static void draw_prowler_action(ImDrawList *d, float x, float y, unsigned id){
    if(!id || id >= sizeof(prowler_action_art)/sizeof(prowler_action_art[0]))return;
    draw_packed_sprite(d, &prowler_icons[prowler_action_art[id].icon], x, y, 32, 96,
                       item_palette[prowler_action_art[id].color]);
}
