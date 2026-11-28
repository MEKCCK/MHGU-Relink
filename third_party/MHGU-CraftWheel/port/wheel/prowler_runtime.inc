/* MHGU 1.4.0. The original Support selector at main+65a17c iterates the
   local Prowler's eight equipped slots. Passive and learned moves occupy
   other arrays. The selected Palico can change while this module is loaded. */
static uint32_t prowler_owner, prowler_generation;
static const uint8_t *prowler_record;
static uint8_t prowler_unique[8];
static uint8_t *prowler_actor;

static bool prowler_supported(void){
 return *(const uint32_t*)(base+0x65a1c4)==0xebea3a65 &&
        *(const uint32_t*)(base+0x654750)==0xebfbf1dd &&
        *(const uint32_t*)(base+0x6547c4)==0xeaf17a53;
}

static uint8_t *prowler_quest_actor(void){
 uint8_t *session=*(uint8_t**)(base+0x188ca48);
 void *players=*(void**)(base+0x1885b58);
 if(!session || !players || *(uint32_t*)(session+0x30)>=4)return 0;
 /* Native local-player resolver used by both the action check and selector.
    It rejects hunters, remote players and missing actors. */
 uint8_t *actor=((uint8_t*(*)(void*))(base+0x277334))(players);
 /* During a scene change the old lobby player can still occupy the slot. */
 return actor && *(uintptr_t*)actor!=base+0x1748974?actor:0;
}

static uint8_t *prowler_actor_record(uint8_t *actor){
 uint8_t *data=actor?*(uint8_t**)(actor+0x13c4):0;
 return data?data+4:0;
}

static bool prowler_same_record(const uint8_t *record){
 if(!record || record!=prowler_record)return false;
 /* The original Airou identifier accessor at main+e7b74 returns these
    eight bytes. A new cat can reuse the previous cat's actor and record. */
 for(unsigned i=0;i<8;i++)if(record[0x274+i]!=prowler_unique[i])return false;
 return true;
}

static bool prowler_read(uint32_t actions[8]){
 for(unsigned i=0;i<8;i++)actions[i]=0;
 prowler_actor=0;
 if(prowler_supported()){
  if(quest)prowler_actor=prowler_quest_actor();
  else {
   uint8_t *session=*(uint8_t**)(base+0x188ca48);
   void *players=*(void**)(base+0x1885b58);
   if(session && players && *(uint32_t*)(session+0x1c)==2 && *(uint32_t*)(session+0x30)<4){
    uint8_t *local=((uint8_t*(*)(void*))(base+0x2772c4))(players);
    /* uPlayerLobbyAirou. The shared uPlayer accessor at 27c708 takes
       this actor's AirouParam from +13c4 and skips its four-byte header. */
    if(local && *(uintptr_t*)local==base+0x1748974 && (local[0x13cc]&4))prowler_actor=local;
   }
  }
 }
 uint8_t *record=prowler_actor_record(prowler_actor);
 if(!record){
  prowler_record=0;
  prowler_owner=0;
  return prowler_actor!=0;
 }
 if(!prowler_same_record(record)){
  prowler_record=record;
  for(unsigned i=0;i<8;i++)prowler_unique[i]=record[0x274+i];
  if(!++prowler_generation)++prowler_generation;
  prowler_owner=prowler_generation;
 }
 for(unsigned i=0;i<8;i++){
  unsigned id=record[0x244+i];  /* main+e8b60 */
  if(id && id<=56)actions[i]=id;
 }
 return true;
}

static unsigned prowler_resolve(uint8_t *actor,unsigned id){
 if(id!=46)return id;
 void *otomo=*(void**)(base+0x1885468);
 if(!otomo || !actor)return id;
 return ((unsigned(*)(void*,unsigned,void*))(base+0x264e7c))(otomo,id,actor);
}

static unsigned prowler_display_action(unsigned id){
 if(id!=46 || !prowler_actor || !quest || !prowler_supported())return id;
 uint8_t *actor=prowler_quest_actor();
 if(actor!=prowler_actor || !prowler_same_record(prowler_actor_record(actor)))return id;
 return prowler_resolve(actor,id)==0xfe?57:id;
}

static bool prowler_use(unsigned slot,unsigned expected_id){
 if(!quest || !is_quest() || !expected_id || expected_id>56 || slot>=8 ||
    !prowler_actor || !prowler_supported())return false;
 uint8_t *actor=prowler_quest_actor();
 uint8_t *record=prowler_actor_record(actor);
 if(actor!=prowler_actor || !prowler_same_record(record) ||
    record[0x244+slot]!=expected_id || !*(void**)(base+0x1885468) ||
    (*(uint32_t*)(actor+0x2500)&0x800))return false;
 /* Stock cUIOCPanelSupportUse (654708 / 65478c): resolve the current
    Beast form, run the original equipped/action/gauge checks, then send
    the original request. Gameplay owns gauge spending and animation. */
 unsigned effective=prowler_resolve(actor,expected_id);
 if(((unsigned(*)(unsigned))(base+0x550ecc))(effective))return false;
 ((void(*)(void*,unsigned))(base+0x2b3118))(actor,expected_id);
 return true;
}
