#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "../presets/vendor/cimgui-1.4/cimgui.h"
#include <stdint.h>
#include <stdbool.h>
#include "../presets/item_names.inc"
#include "recipes.inc"
#include "icon_runs.inc"
#include "button_runs.inc"
#include "item_names_en.inc"
typedef struct {uint16_t id,qty;} Slot;
typedef struct {uint32_t visible,editing,picker,direction,pick_index,targets[8],hold_since,holding,armed,latch,action,action_item;} WheelState;
extern WheelState wheel;
extern unsigned release_trigger,open_key;
extern unsigned preset_row,ui_focus,detail_index,quick_enabled;
extern void wheel_reset(void);
extern void wheel_enter_settings(unsigned);
extern void wheel_step(unsigned,unsigned,unsigned,int,int,unsigned,const Slot*);
extern float wheel_open_progress(unsigned);
extern unsigned wheel_centered;
extern unsigned playing_prowler,wheel_cat_trigger,wheel_cat_enabled;
extern uint32_t cat_actions[8];
extern void wheel_cancel_gesture(void);
extern unsigned wheel_trigger_now(void);
/* Forge 1.5 shared-JIT ABI, including the per-hook context cell. */
typedef struct {void*target,*detour,*original,*context,*ctx_cell;bool has_ctx;} Hook;
typedef struct {uint16_t major,minor,patch;char name[64];} Init;
typedef struct {uint64_t sample,buttons;int32_t lx,ly,rx,ry;uint32_t attributes;} Pad;
extern uint32_t forge_mem_getMainTextAddr(void);
extern Hook forge_hook_create(void*,void*,void**);
extern bool forge_input_isDown(int);
extern void forge_input_getStickR(float*,float*);
extern void forge_log(int,const char*,...);
extern bool forge_fs_mountSaveData(void);
extern void forge_fs_unmountSaveData(void);
extern uint64_t nnosGetSystemTick(void);
extern uint64_t nnosGetSystemTickFrequency(void);
extern void pad_full(Pad*,const unsigned*) __asm__("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
extern void pad_hand(Pad*,const unsigned*) __asm__("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj");
extern void pad_dual(Pad*,const unsigned*) __asm__("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj");
static void(*full_orig)(Pad*,const unsigned*),(*hand_orig)(Pad*,const unsigned*),(*dual_orig)(Pad*,const unsigned*);
static uint32_t base,last_held,mode_logged=~0u,filter_logs,release_mask;
static uint64_t frequency,start_tick;
static bool loaded,quest,ready;
#include "prowler_options.h"

static uint32_t use_mode,mode_sequence; static unsigned icon_style=1;
#include "wheel_pages.inc"
static unsigned opacity=3; static bool pc_alpha;
static ImU32 pc_color(ImU32 c){
 unsigned level=playing_prowler?cat_opacity:opacity;
 return pc_alpha?(c&0xffffff)|((((c>>24)*(level+1))/4)<<24):c;
}
static uint32_t preview_targets[8];
static bool previewing;
static unsigned english,quick_key=9,settings_sequence;
static bool mapping,mapping_wait,mapping_open_key,help_open,info_open;
static const char*mapping_error;
static ImFont*font;
static const char*status="配置快捷轮盘";
#include "file_helpers.inc"
void*memset(void*p,int c,unsigned n){volatile unsigned char*d=p;while(n--)*d++=(unsigned char)c;return p;}
unsigned long long __aeabi_ldiv0(unsigned long long n){(void)n;return 0;}
static Slot*pouch(void){uint8_t*p=*(uint8_t**)(base+0x18584b8);return p?(Slot*)(p+0x1a):0;}
static bool is_quest(void){void*r=*(void**)(base+0x188ca48);return r&&((int(*)(void*))(base+0x3f8634))(r)==1;}
static unsigned count(Slot*p,unsigned id){unsigned n=0;for(unsigned i=0;i<40;i++)if(p[i].id==id)n+=p[i].qty;return n;}
/* Full and material shortage are independent; space/unknown recipes are not
   material shortages. X denotes zero stock, independent of crafting materials. */
static unsigned item_state(unsigned id,Slot*p){
 if(!id||id>=2991||!p)return 0;
 unsigned held=count(p,id),state=(item_limits[id]&&held>=item_limits[id]?1:0)|(!held?4:0);bool found=false,enough=false;
 for(unsigned i=0;i<183;i++)if(wheel_recipes[i].result==id){
  found=true;unsigned a=count(p,wheel_recipes[i].a),b=count(p,wheel_recipes[i].b);
  if(wheel_recipes[i].a==wheel_recipes[i].b?a>=2:a&&b){enough=true;break;}
 }
 if(found&&!enough)state|=2;return state;
}
static int choose_recipe(unsigned id,Slot*p){
 if(!id||id>=2991||!p)return -1;
 unsigned current=count(p,id),limit=item_limits[id];if(!limit||current>=limit)return -1;
 bool free=false;for(unsigned j=0;j<40;j++)if(!p[j].id||!p[j].qty)free=true;
 for(unsigned i=0;i<183;i++)if(wheel_recipes[i].result==id){
  unsigned a=count(p,wheel_recipes[i].a),b=count(p,wheel_recipes[i].b);
  if(a&&b&&(free||current||a==1||b==1))return (int)i;
 }
 return -1;
}
static bool craft(unsigned target){
 Slot*p=pouch();int i=choose_recipe(target,p);if(i<0){status="材料不足、已达上限或无法调和";return false;}
 void*meta=*(void**)(base+0x18583a8);if(!meta)return false;
 const WheelRecipe*r=&wheel_recipes[i];
 uint8_t*recipe=((uint8_t*(*)(void*,unsigned,unsigned))(base+0x19463c))(meta,r->a,r->b);
 if(!recipe||*(uint32_t*)(recipe+0x10)!=target){status="配方校验失败";return false;}
 Slot snapshot[50];copy(snapshot,p,200);
 uint32_t result=((uint32_t(*)(void*,unsigned,unsigned,unsigned))(base+0x194474))(meta,r->a,r->b,0);
 unsigned id=result&65535,n=result>>16;if(!id||id>=2991||!n||n>99){status="调和结果无效";return false;}
 void*bag=(uint8_t*)p-0x1a;
 int(*add)(void*,unsigned,int,unsigned)=(void*)(base+0x19b7fc);
 unsigned old_a=count(p,r->a),old_b=count(p,r->b);
 add(bag,r->a,-1,0);add(bag,r->b,-1,0);
 if(count(p,r->a)!=old_a-(r->a==r->b?2:1)||count(p,r->b)!=old_b-(r->a==r->b?2:1)){copy(p,snapshot,200);status="材料扣除失败，已取消";return false;}
 unsigned before=count(p,id),room=item_limits[id]>before?item_limits[id]-before:0;
 if(n>room)n=room;
 if(!n){copy(p,snapshot,200);status="道具袋没有空间";return false;}
 add(bag,id,(int)n,0);
 if(count(p,id)!=before+n){copy(p,snapshot,200);status="道具袋无法收纳，已取消";return false;}
 bool success=id==target;
 if(success)((void(*)(void*,void*))(base+0x194c94))(meta,recipe);
 ((void(*)(unsigned,unsigned))(base+0x54f260))(0,success?0xb:0xc);
 status=success?"调和成功":"调和失败";
 forge_log(3,"[CraftWheel] craft target=%u result=%u qty=%u success=%u",target,id,n,success);return true;
}
#include "native_item_use.inc"
/* Same request as cUIOCPanelItemUse's native activation handler (0x64fe2c),
   not an item effect or inventory decrement. The player state machine remains
   responsible for animations, restrictions, effects and actual consumption. */
static bool request_use(unsigned id){
 Slot*p=pouch();if(!quest||!p||!direct_item_type(id)||!count(p,id))return false;
 if(*(uint32_t*)(base+0x2772c4)!=0xe92d4010||*(uint32_t*)(base+0x64fe60)!=0xe303229c){status="此版本的道具使用接口未验证";return false;}
 uint8_t*player=wheel_local_player();if(!player)return false;
 if(player[0x13cc]&4){status="当前无法使用道具";return false;}
 if(((int(*)(void*,unsigned,unsigned))method(player,0x5bc/4))(player,id,0)!=1){status="当前无法使用该道具";return false;}
 volatile uint32_t*flags=(volatile uint32_t*)(player+0x2500);
 if(*flags&0x400){status="已有道具使用请求";return false;}
 *(volatile uint16_t*)(player+0x329c)=(uint16_t)id;*flags|=0x400;
 status="已请求使用道具";forge_log(3,"[CraftWheel] native item use requested id=%u",id);return true;
}
typedef struct {uint32_t magic,version,seq,targets[8],crc;} Disk;
typedef struct {uint32_t magic,seq,left,crc;} PositionDisk;
static const char*position_paths[]={"savedata:/gu_wheel_position_a.bin","savedata:/gu_wheel_position_b.bin"};
static const char*mode_paths[]={"savedata:/gu_wheel_mode_a.bin","savedata:/gu_wheel_mode_b.bin"};
static bool read_mode(const char*path,PositionDisk*d){
 void*f=open_file(path,0x11);if(!f)return false;
 unsigned size=file_size(f),n=0;
 if(size==sizeof(*d))n=file_read(f,d,sizeof(*d));
 close_file(f);return n==sizeof(*d)&&d->magic==0x314d5747&&d->left<=1&&crc(d,sizeof(*d)-4)==d->crc;
}
static void load_mode(void){
 PositionDisk a,b;bool va=read_mode(mode_paths[0],&a),vb=read_mode(mode_paths[1],&b);
 PositionDisk*d=vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:0;
 use_mode=d?d->left:0;mode_sequence=d?d->seq:0;
}
static bool save_mode(unsigned value){
 PositionDisk d={0x314d5747,mode_sequence+1,value,0};d.crc=crc(&d,sizeof(d)-4);
 if(!storage_begin())return false;
 const char*path=mode_paths[d.seq&1];void*f=open_file(path,0x13);bool ok=false;
 if(f){unsigned n=file_write(f,&d,sizeof(d));close_file(f);PositionDisk check;ok=n==sizeof(d)&&read_mode(path,&check)&&check.seq==d.seq&&check.left==value&&check.crc==d.crc;}
 storage_end();if(ok){use_mode=value;mode_sequence=d.seq;}return ok;
}
#include "wheel_position.inc"
typedef struct {uint32_t magic,version,seq,targets[4],crc;} LegacyDisk;
static const char*paths[]={"savedata:/gu_craft_wheel8_a.bin","savedata:/gu_craft_wheel8_b.bin"};
static const char*legacy_paths[]={"savedata:/gu_craft_wheel_a.bin","savedata:/gu_craft_wheel_b.bin"};
static bool read_disk(const char*path,Disk*d){
 void*f=open_file(path,0x11);if(!f)return false;
 unsigned sz=file_size(f);
 unsigned n=0;
 if(sz==sizeof(*d))n=file_read(f,d,sizeof(*d));
 else if(sz==sizeof(LegacyDisk)){
  LegacyDisk old;unsigned got=file_read(f,&old,sizeof(old));
  if(got==sizeof(old)&&old.magic==0x31574347&&old.version==1&&crc(&old,sizeof(old)-4)==old.crc){
   d->magic=old.magic;d->version=2;d->seq=old.seq;
   for(unsigned i=0;i<8;i++)d->targets[i]=i<4?old.targets[i]:0;
   d->crc=crc(d,sizeof(*d)-4);n=sizeof(*d);
  }
 }
 close_file(f);
 if(n!=sizeof(*d)||d->magic!=0x31574347||d->version!=2||crc(d,sizeof(*d)-4)!=d->crc)return false;
 for(unsigned i=0;i<8;i++)if(d->targets[i]>=2991)return false;return true;
}
#include "wheel_presets.inc"
#include "prowler_settings.inc"
#include "wheel_settings.inc"
static bool load_config(void){
 if(!storage_begin())return false;
 Disk a,b;bool va=read_disk(paths[0],&a),vb=read_disk(paths[1],&b);
 if(!va&&!vb){va=read_disk(legacy_paths[0],&a);vb=read_disk(legacy_paths[1],&b);if(va||vb)forge_log(3,"[CraftWheel] imported four-way settings; diagonal slots empty");}
 Disk*d=vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:0;
 load_presets(d);load_position();load_mode();load_extra();load_settings();load_cat_options();storage_end();return true;
}
#include "plus_menu.inc"
#include "prowler_runtime.inc"
#include "prowler_wheel.inc"
#include "settings_touch.inc"
static bool settings_quest;
static unsigned pause_held;
static void reset_settings_view(void){
 settings_quest=quest;
 preset_row=presets.active;ui_focus=detail_index=0;
 previewing=mapping=help_open=info_open=position_editing=false;
 mapping_error=0;
 status="当前正在使用的轮盘";
}
static void filter(Pad*p,uint32_t caller){
 if(caller<base||caller>=base+0x13f1dec)return;
 bool q=is_quest();
 if(wheel.editing){p->buttons=0;p->lx=p->ly=p->rx=p->ry=0;return;}
 if(plus_menu_active()){
  p->buttons&=~(uint64_t)release_mask;
  return;
 }
 if(q&&wheel.visible&&(p->buttons&(1ull<<open_key))){
  p->buttons&=~(0xfull<<20); /* Npad digital right-stick directions. */
  p->rx=p->ry=0;if(filter_logs<2){forge_log(3,"[CraftWheel] game camera input suppressed caller=%08X",caller);filter_logs++;}}
 if(q&&!playing_prowler&&quick_enabled&&(p->buttons&(1ull<<open_key)))p->buttons&=~(1ull<<quick_key);

 p->buttons&=~(uint64_t)release_mask;
}
static void full_hook(Pad*p,const unsigned*port){uint32_t lr=(uint32_t)__builtin_return_address(0);full_orig(p,port);filter(p,lr);}
static void hand_hook(Pad*p,const unsigned*port){uint32_t lr=(uint32_t)__builtin_return_address(0);hand_orig(p,port);filter(p,lr);}
static void dual_hook(Pad*p,const unsigned*port){uint32_t lr=(uint32_t)__builtin_return_address(0);dual_orig(p,port);filter(p,lr);}
void init(void){}void fini(void){}
void forge_onLoad(Init*p){p->major=1;p->minor=5;p->patch=1;const char n[]="GU CraftWheel Switch 0.50";for(unsigned i=0;i<sizeof(n);i++)p->name[i]=n[i];}
void forge_onInit(void){
 base=forge_mem_getMainTextAddr();wheel_reset();frequency=nnosGetSystemTickFrequency();start_tick=nnosGetSystemTick();
 if(*(uint32_t*)(base+0x194474)!=0xe92d47f0||*(uint32_t*)(base+0x19b7fc)!=0xe92d4ff0||*(uint32_t*)(base+0x3f8634)!=0xe590001c||*(uint32_t*)(base+0x560d3c)!=0xe59f0080){forge_log(3,"[CraftWheel] unsupported executable; hooks disabled");return;}
 forge_hook_create(pad_full,full_hook,(void**)&full_orig);forge_hook_create(pad_hand,hand_hook,(void**)&hand_orig);forge_hook_create(pad_dual,dual_hook,(void**)&dual_orig);
 ready=full_orig&&hand_orig&&dual_orig;
 forge_log(3,"[CraftWheel] SWITCH-0.50 input gates ready=%u",ready);
 if(ready)plus_menu_install(base);
 forge_log(1,"[CraftWheel] SWITCH-0.50 frequency=%u",(unsigned)frequency);
}
void forge_onUpdate(float dt){
 (void)dt; if(!ready)return;Slot*p=pouch();if(!frequency)return;
 quest=is_quest();void*r=*(void**)(base+0x188ca48);unsigned mode=r?*(uint32_t*)((uint8_t*)r+0x1c):~0u;
 if(mode!=mode_logged){forge_log(3,"[CraftWheel] mode=%u quest=%u",mode,quest);mode_logged=mode;}
 refresh_prowler_wheel();
 if(!p&&!playing_prowler)return;
 if(!loaded){loaded=load_config();if(!loaded)return;}
 wheel_cat_trigger=cat_trigger;
 wheel_cat_enabled=cat_enabled;
 uint32_t held=0;for(unsigned i=0;i<16;i++)if(forge_input_isDown(i))held|=1u<<i;
 release_mask&=held;
 unsigned pressed=held&~last_held;last_held=held;
 if(plus_menu_take_open()){
  wheel_enter_settings(quest);
  reset_settings_view();
  release_mask|=held;
  pause_held=0;
  return;
 }
 if(plus_menu_active()&&!wheel.editing){
  pause_held=held;
  wheel_step(quest,0,0,0,0,0,p);
  return;
 }
 if(pause_held){
  release_mask|=pause_held&held;
  pause_held=0;
 }
 held&=~release_mask;
 pressed&=~release_mask;
 if(wheel.editing && settings_quest!=quest){
  wheel.editing=wheel.visible=0;
  reset_settings_view();
 }
 settings_touch_input(wheel.editing && !wheel.picker && !position_editing &&
                      !mapping && !help_open && !info_open, &pressed);
 if(position_editing){
  if(wheel.editing&&position_quest==quest){update_position_edit(held,pressed);return;}
  position_editing=false;
 }
 if(wheel.editing&&help_open){if(pressed&2)help_open=false;return;}
 if(wheel.editing&&info_open){if(pressed&2)info_open=false;return;}
 if(wheel.editing&&mapping){
  if(mapping_wait){if(!held)mapping_wait=false;return;}
  if(!pressed)return;
  if(held!=pressed||(pressed&(pressed-1))){
   mapping_error="请松开后单独按一个键";mapping_wait=true;return;
  }
  unsigned k=0;while(!(pressed&(1u<<k)))k++;
  if((mapping_open_key&&k==10)||(!mapping_open_key&&k==4)){
   mapping=false;mapping_error=0;release_mask|=held;return;
  }
  if(k==5||(!mapping_open_key&&k==open_key)){
   mapping_error="此键已保留，请换一个键";mapping_wait=true;return;
  }
  unsigned secondary=mapping_open_key ? switch_key_for_opener(quick_key,k,open_key) : k;
  bool moved_secondary=mapping_open_key && secondary!=quick_key;
  bool saved=mapping_open_key?save_bindings(english,quick_enabled,secondary,k):save_settings(english,quick_enabled,k);
  if(!saved){mapping_error="设置保存失败";mapping_wait=true;return;}
  status=moved_secondary?"呼出键和切换键已调整":mapping_open_key?"呼出按键已保存":"快捷键已保存";
  mapping=false;mapping_error=0;release_mask|=held;return;
 }
 if(quest&&!playing_prowler&&!wheel.editing&&quick_enabled&&(held&(1u<<open_key))&&(pressed&(1u<<quick_key))){
  quick_next();wheel.holding=0;wheel.latch=2;release_mask|=1u<<quick_key;return;
 }
 float x=0,y=0;forge_input_getStickR(&x,&y);unsigned now=(unsigned)((nnosGetSystemTick()-start_tick)*1000/frequency);
 uint32_t old[8];copy(old,wheel.targets,32);
 bool was_editing=wheel.editing;
 bool editing_preview=was_editing&&previewing;
 if(editing_preview)copy(wheel.targets,preview_targets,32);
 wheel_step(quest,held,pressed,(int)(x*32767),(int)(y*32767),now,p);
 if(editing_preview){copy(preview_targets,wheel.targets,32);copy(wheel.targets,old,32);}
 if(!wheel.editing)previewing=false;
 if(!was_editing&&wheel.editing)reset_settings_view();
 if(was_editing&&!wheel.editing)release_mask=held&0xffff;
 if(wheel.action==14)use_prowler_selection();
 if(wheel.action==1&&quest&&!playing_prowler){
  if(release_trigger){if(!wheel.action_item||!craft(wheel.action_item))wheel.latch=2;}
  else if(use_mode){if(!wheel.action_item)wheel.latch=2;else if(count(p,wheel.action_item)&&request_use(wheel.action_item))wheel.latch=2;else if(!craft(wheel.action_item))wheel.latch=2;}
  else if(!wheel.action_item||!craft(wheel.action_item))wheel.latch=2;
 }
 if(wheel.action==2||wheel.action==5){if(editing_preview)status="预设已修改，按[LEFT]保存";else if(save_preset(presets.active,wheel.targets))status=wheel.action==5?"已清除所选方向":"已登录该方向";else{copy(wheel.targets,old,32);status="保存失败，原配置已保留";}}

 if(wheel.action==4){if(save_mode(!use_mode))status="轮盘模式已保存";else status="模式保存失败，原设置已保留";}
 if(wheel.action==6){
  select_preset_row((preset_row+(wheel.action_item?1:5))%6);
  detail_index=0;
 }
 if(wheel.action==13)change_cat_option(wheel.action_item);
 if(wheel.action==7){
  if(preset_row<4)status=save_preset(preset_row,previewing?preview_targets:wheel.targets)?"预设已保存":"预设保存失败，原预设已保留";
 }
 if(wheel.action==8){
  if(!(presets.valid&(1u<<preset_row)))status="该预设尚未保存";
  else activate_preset(preset_row);
 }
 if(wheel.action==9&&quest&&!playing_prowler&&use_mode&&wheel.action_item)request_use(wheel.action_item);
 if(wheel.action==12&&quest&&!playing_prowler&&use_mode&&wheel.action_item)request_use(wheel.action_item);
 if(wheel.action==10)status=save_trigger((release_trigger+1)%3)?"触发方式已保存":"触发方式保存失败";
 if(wheel.action==11){
  switch(wheel.action_item){
   case 0:status=save_mode(!use_mode)?"轮盘模式已保存":"设置保存失败";break;
   case 1:begin_position_edit();break;
   case 2:status=save_settings(!english,quick_enabled,quick_key)?"语言已切换":"设置保存失败";break;
   case 3:status=save_settings(english,!quick_enabled,quick_key)?"快捷切换设置已保存":"设置保存失败";break;
   case 4:case 10:
    if(wheel.action_item==10||quick_enabled){
     mapping_open_key=wheel.action_item==10;mapping_error=0;mapping=mapping_wait=true;
    }break;
   case 5:status=save_trigger((release_trigger+1)%3)?"触发方式已保存":"设置保存失败";break;
   case 6:help_open=true;break;
   case 7:info_open=true;break; case 9:{unsigned old=icon_style;icon_style=(icon_style+1)%3;if(!save_settings(english,quick_enabled,quick_key))icon_style=old;break;} case 8:{unsigned old=opacity;opacity=(opacity+1)%4;if(!save_settings(english,quick_enabled,quick_key))opacity=old;break;}
  }
 }
}
void forge_onImGuiInit(void*ctx,void*a,void*f){
 (void)ctx;(void)a;(void)f;
 ImFontAtlas*atlas=igGetIO_Nil()->Fonts;
 for(int i=0;i<atlas->Fonts.Size;i++)if(atlas->Fonts.Data[i]==font && font)return;
 ImFontConfig*config=ImFontConfig_ImFontConfig();
 if(config){config->OversampleH=1;config->OversampleV=1;}
 font=ImFontAtlas_AddFontFromFileTTF(atlas,"app:/nativeNX/plugins/gu_craft_wheel_font.ttc",22,config,0);
 if(config)ImFontConfig_destroy(config);
 forge_log(3,"[CraftWheel] font=%08X",(uint32_t)font);
}
static float scale,origin_x,origin_y;
static ImVec2_c v(float x,float y){return(ImVec2_c){origin_x+x*scale,origin_y+y*scale};}
/* The NVN backend projects using IO.DisplaySize. Viewport.Size can differ
   across mobile integrations; using it alone enlarges/crops the overlay.
   Fit the 1280x720 design into the same logical area as the projection.
   FramebufferScale is a pixel density, not another UI size multiplier. */
static bool fit_display(void){
 ImGuiIO*io=igGetIO_Nil();if(!io)return false;
 float w=io->DisplaySize.x,h=io->DisplaySize.y;
 if(!(w>=100&&w<=32768&&h>=100&&h<=32768))return false;
 float sx=w/1280.0f,sy=h/720.0f;scale=sx<sy?sx:sy;
 origin_x=(w-1280.0f*scale)*0.5f;origin_y=(h-720.0f*scale)*0.5f;
 static unsigned last_w,last_h;
 if(last_w!=(unsigned)w||last_h!=(unsigned)h){
  last_w=(unsigned)w;last_h=(unsigned)h;
  ImGuiViewport*vp=igGetMainViewport();
  forge_log(3,"[CraftWheel] UI display=%ux%u viewport=%ux%u scale/1000=%u",last_w,last_h,vp?(unsigned)vp->Size.x:0,vp?(unsigned)vp->Size.y:0,(unsigned)(scale*1000));
 }
 return true;
}
static void rect(ImDrawList*d,float x,float y,float w,float h,ImU32 c){ImDrawList_AddRectFilled(d,v(x,y),v(x+w,y+h),pc_color(c),2*scale,0);}
#include "wheel_language.inc"
static void outlined_text(ImDrawList*d,float x,float y,const char*s,ImU32 c,float size,float stroke){
 // ImGui rounds the requested size before baking. Animation endpoints can
 // produce a positive sub-pixel size which rounds to zero and asserts.
 float pixels=size*scale;if(!(pixels>=1.0f&&pixels<4096.0f))return;
 ImFont*f=font?font:igGetFont();if(!f)return;
 /* Limit only our own font. Text keeps its layout size while ImGui scales
    glyphs from these two caches across animation and viewport changes. */
 if(font && !(font->Flags & ImFontFlags_LockBakedSizes)){
  ImFont_GetFontBaked(font,24.0f,-1.0f);
  ImFont_GetFontBaked(font,36.0f,-1.0f);
  font->Flags |= ImFontFlags_LockBakedSizes;
  forge_log(1,"[CraftWheel] wheel font cache ready (24/36 px)");
 }
 for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)if(dx||dy)
  ImDrawList_AddText_FontPtr(d,f,size*scale,v(x+dx*stroke,y+dy*stroke),pc_color(c&0xff000000u),s,0,0,0);
 ImDrawList_AddText_FontPtr(d,f,size*scale,v(x,y),pc_color(c),s,0,0,0);
}
static void raw_text(ImDrawList*d,float x,float y,const char*s,ImU32 c,float size){outlined_text(d,x,y,s,c,size,1);}
static bool dim_button;
static void dpad(ImDrawList*d,float x,float y,unsigned key,float size){
 // Raised white cross with a black rim; only the indicated arm is dark.
 // Three overlapping rectangles fill the concave cross without convex-fill artefacts.
 ImDrawList_AddRectFilled(d,v(x+size*.3f,y),v(x+size*.7f,y+size),pc_color(0xff050505),2*scale,0);
 ImDrawList_AddRectFilled(d,v(x,y+size*.3f),v(x+size,y+size*.7f),pc_color(0xff050505),2*scale,0);
 ImU32 face=dim_button?0xff777777:0xffeeeeee;
 ImDrawList_AddRectFilled(d,v(x+size*.36f,y+size*.06f),v(x+size*.64f,y+size*.94f),pc_color(face),scale,0);
 ImDrawList_AddRectFilled(d,v(x+size*.06f,y+size*.36f),v(x+size*.94f,y+size*.64f),pc_color(face),scale,0);
 float xx=x+size*.4f,yy=y+size*.4f;
 if(key==12)xx=x+size*.09f;if(key==14)xx=x+size*.7f;
 if(key==13)yy=y+size*.09f;if(key==15)yy=y+size*.7f;
 ImDrawList_AddRectFilled(d,v(xx,yy),v(xx+size*.2f,yy+size*.2f),pc_color(0xff393939),scale,0);
}
#include "native_atlas.inc"
static void atlas_icon(ImDrawList*d,float x,float y,unsigned key,float size){
 if(key>=18 || icon_style>=3)return;
 float physical=size*scale;
 unsigned level=physical<24?0:physical<48?1:physical<96?2:3;
 draw_packed_sprite(d,&control_sprites[icon_style*18+key][level],x,y,size,16u<<level,
                    dim_button?0xff888888:0xffffffff);
}
static void button(ImDrawList*d,float x,float y,unsigned key,float size){atlas_icon(d,x-size*.10f,y-size*.10f,key,size*1.20f);}
static float text_width(const char*s,float size){float n=0;for(const unsigned char*p=(const unsigned char*)s;*p;p++)if((*p&0xc0)!=0x80)n+=*p<128?size*.54f:size;return n;}
/* Bracketed control names are replaced by GU's original button artwork. */
static void text(ImDrawList*d,float x,float y,const char*src,ImU32 c,float size){
 const char*s=translate(src);char buf[256];unsigned n=0;
 while(*s){
  if(*s=='['){
   static const char*keys[]={"A]","B]","X]","Y]","L3]","R3]","L]","R]","ZL]","ZR]","+]","-]","LEFT]","UP]","RIGHT]","DOWN]","RS]","OPEN]"};
   int key=-1;unsigned len=0;
   for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++){const char*a=s+1,*b=keys[i];while(*b&&*a==*b){a++;b++;}if(!*b){key=i==17?open_key:i;len=(unsigned)(a-s);break;}}
   if(key>=0){if(n){buf[n]=0;raw_text(d,x,y,buf,c,size);x+=text_width(buf,size);n=0;}button(d,x,y-1,key,size+3);x+=size*1.2f+7;s+=len;continue;}
  }
  buf[n++]=*s++;if(n==sizeof(buf)-1){buf[n]=0;raw_text(d,x,y,buf,c,size);x+=text_width(buf,size);n=0;}
 }
 if(n){buf[n]=0;raw_text(d,x,y,buf,c,size);}
}
static void number(char*out,unsigned n){char t[12];unsigned z=0;do{t[z++]='0'+n%10;n/=10;}while(n);unsigned i=0;while(z)out[i++]=t[--z];out[i]=0;}
static void item_icon(ImDrawList*d,float x,float y,unsigned id){
 if(id>=2991)return;
 unsigned icon=icon_indices[id],color=icon_colors[id];
 if(icon<100)draw_packed_sprite(d,&item_sprites[icon],x,y,32,96,item_palette[color<=16?color:0]);
}
#include "prowler_actions.inc"
#include "wheel_render.inc"

