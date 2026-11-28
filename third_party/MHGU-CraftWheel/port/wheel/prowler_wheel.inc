/* Native equipped entries are in HUD order. As in the original action bar,
   empty entries are skipped. Keep the original slot for native activation. */
static uint8_t cat_source_slots[8];
static uint32_t wheel_cat_owner;
static uint8_t cat_low_gauge;

/* The original quest action validator at main+1218978 computes the effective
   cost (including skill and song discounts), then asks the actor's gauge. */
static bool prowler_gauge_short(unsigned id){
 uint8_t *manager=*(uint8_t**)(base+0x1885468);
 if(!manager || !*(void**)(manager+0x2b3c) || !prowler_actor || !prowler_record ||
    *(const uint32_t*)(base+0x1218998)!=0xebbb40d1 ||
    *(const uint32_t*)(base+0x12189a4)!=0xe5902470)return false;
 uint8_t discount=0;
 unsigned effective=prowler_resolve(prowler_actor,id);
 unsigned cost=((unsigned(*)(const void*,unsigned,uint8_t*,void*))(base+0xe8ce4))
                   (prowler_record,effective,&discount,prowler_actor);
 if(!cost)return false;
 return !((unsigned(*)(void*,unsigned))method(prowler_actor,0x470/4))(prowler_actor,cost);
}
static bool refresh_prowler_wheel(void){
 uint32_t native_actions[8]={0};
 bool active=prowler_read(native_actions);
 bool changed=playing_prowler!=active || wheel_cat_owner!=prowler_owner;
 unsigned next=0;
 cat_low_gauge=0;
 if(active){
  for(unsigned slot=0;slot<8;slot++){
   unsigned id=native_actions[slot];
   if(!id)continue;
   if(cat_actions[next]!=id || cat_source_slots[next]!=slot)changed=true;
   if(quest && wheel.visible && (!wheel.editing || (position_editing && cat_position_edit)) &&
      prowler_gauge_short(id))cat_low_gauge|=(uint8_t)(1u<<next);
   cat_actions[next]=id;
   cat_source_slots[next++]=slot;
  }
 }
 for(;next<8;next++){
  if(cat_actions[next])changed=true;
  cat_actions[next]=0;
  cat_source_slots[next]=8;
 }
 playing_prowler=active;
 wheel_cat_owner=prowler_owner;
 if(changed)wheel_cancel_gesture();
 return changed;
}

static void use_prowler_selection(void){
 unsigned dir=wheel.direction;
 if(!quest || !playing_prowler || !cat_enabled || dir>=8 || !wheel.action_item ||
    cat_actions[dir]!=wheel.action_item || cat_source_slots[dir]>=8)return;
 prowler_use(cat_source_slots[dir],wheel.action_item);
}
