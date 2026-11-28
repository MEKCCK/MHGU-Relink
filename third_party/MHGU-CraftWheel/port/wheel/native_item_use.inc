/* itemData.itm, 44-byte records, byte 12. ID 93 is the item bar's
   "no item" placeholder. Mushrooms additionally pass the game's skill
   checks in main+0x2fabd4 via the normal item-use virtual method. */
static bool direct_item_type(unsigned id){
 static const uint16_t ranges[][2]={
  {8,33},{38,39},{44,54},{56,68},{70,78},{83,84},{88,92},{94,97},
  {246,248},{251,252},{255,269},{272,273},{282,283},{292,301},{303,305},
  {317,317},{359,363},{371,372},{385,385},{1716,1733},{1735,1735},
  {2054,2058},{2098,2098},{2104,2104},{2637,2637},{2928,2928}
 };
 for(unsigned i=0;i<sizeof(ranges)/sizeof(ranges[0]);i++){
  if(id<ranges[i][0])return false;
  if(id<=ranges[i][1])return true;
 }
 return false;
}

static uint8_t *wheel_local_player(void){
 void *system=*(void**)(base+0x1885b58);
 return system?((void*(*)(void*))(base+0x2772c4))(system):0;
}
