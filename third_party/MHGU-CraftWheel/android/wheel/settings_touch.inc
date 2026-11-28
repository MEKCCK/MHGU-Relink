#include "touch_layout.inc"


static int menu_touch_hit(float x,float y){
 if(!wheel.editing)return TOUCH_NONE;
 if(position_editing){
  float toolbar=position_toolbar_y();
  if(hit_rect(x,y,584,toolbar+7,120,52))return TOUCH_OK;
  if(hit_rect(x,y,718,toolbar+7,120,52))return TOUCH_CANCEL;
  if(hit_rect(x,y,852,toolbar+7,137,52))return TOUCH_DEFAULT;
  return TOUCH_NONE;
 }
 if(info_open||mapping||help_open){
  if(mapping){
   for(unsigned key=0;key<16;key++){
    TouchPoint p=bind_key_corner(key);
    if(hit_rect(x,y,p.x,p.y,74,54))return TOUCH_BIND+key;
   }
  }
  float top=mapping?465:help_open?460:481;
  return hit_rect(x,y,820,top,150,45)?TOUCH_CANCEL:TOUCH_NONE;
 }
 if(wheel.picker){
  for(unsigned i=0;i<4;i++)if(hit_rect(x,y,touch_pad_centers[i].x-28,touch_pad_centers[i].y-28,56,56))return TOUCH_PAD+i;
  if(hit_circle(x,y,confirm_center,45))return TOUCH_OK;
  if(hit_circle(x,y,back_center,45))return TOUCH_CANCEL;
  if(hit_rect(x,y,925,508,176,52))return TOUCH_CLEAR;
  if(hit_rect(x,y,378,163,524,400)){
   unsigned slot=(wheel.pick_index/16)*16+(unsigned)((y-163)/25);
   if(slot<40)return TOUCH_PICK+slot;
  }
  return TOUCH_NONE;
 }
 if(hit_rect(x,y,48,547,204,54))return TOUCH_CLOSE;
 for(unsigned i=0;i<6;i++)if(hit_rect(x,y,48,164+i*64,204,60)){
  if(i==4)return TOUCH_DETAILS;
  if(i==5)return TOUCH_CAT_SETTINGS;
  return TOUCH_PRESET+i;
 }
 if(preset_row==5){
  for(unsigned row=0;row<4;row++)if(hit_rect(x,y,303,205+row*55,670,49))return TOUCH_DETAIL+row;
  return TOUCH_NONE;
 }
 if(preset_row==4){
  if(hit_rect(x,y,690,149,283,44))return TOUCH_SWITCH;
  if(touch_mode){
   for(unsigned i=0;i<6;i++)if(hit_rect(x,y,303,205+i*55,670,49))return TOUCH_DETAIL+touch_detail_rows[i];
  }else if(hit_rect(x,y,303,194,670,11*33)){
   unsigned row=(unsigned)((y-194)/33);
   return row==4&&!quick_enabled?TOUCH_NONE:TOUCH_DETAIL+row;
  }
 }else{
  if(!touch_mode){
   if(hit_rect(x,y,550,150,135,42))return TOUCH_SAVE_PRESET;
   if(hit_rect(x,y,699,150,135,42))return TOUCH_LOAD_PRESET;
  }
  for(unsigned i=0;i<8;i++)if(hit_circle(x,y,slot_centers[i],40))return TOUCH_SLOT+i;
 }
 return TOUCH_NONE;
}

static void apply_menu_tap(int hit,unsigned *pressed){
 if(hit>=TOUCH_BIND)*pressed=1u<<(hit-TOUCH_BIND);
 else if(hit>=TOUCH_PICK){
  unsigned i=hit-TOUCH_PICK;
  Slot *bag=pouch();
  if(i<40&&bag&&bag[i].id&&bag[i].qty)wheel.pick_index=i;
 }else if(hit>=TOUCH_PAD)*pressed=1u<<(12+hit-TOUCH_PAD);
 else if(hit>=TOUCH_DETAIL){ui_focus=1;detail_index=hit-TOUCH_DETAIL;*pressed=1;}
 else if(hit>=TOUCH_SLOT){ui_focus=0;wheel.direction=hit-TOUCH_SLOT;*pressed=1;}
 else if(hit>=TOUCH_PRESET){
  unsigned index=hit-TOUCH_PRESET;
  if(touch_mode)activate_preset(index);
  else{select_preset_row(index);ui_focus=0;}
 }
 else if(hit==TOUCH_SAVE_PRESET)*pressed=1u<<12;
 else if(hit==TOUCH_LOAD_PRESET)*pressed=1u<<14;
 else if(hit==TOUCH_CLEAR){wheel.picker=0;*pressed=1u<<6;}
 else if(hit==TOUCH_DETAILS||hit==TOUCH_CAT_SETTINGS){previewing=false;preset_row=hit==TOUCH_DETAILS?4:5;ui_focus=1;detail_index=0;if(preset_row==5)status="";}
 else if(hit==TOUCH_SWITCH){status=switch_touch_mode()?(touch_mode?"触屏模式":"手柄模式"):"设置保存失败";*pressed=0;}
 else if(hit==TOUCH_OK)*pressed=1;
 else if(hit==TOUCH_CANCEL&&mapping){mapping=false;mapping_error=0;*pressed=0;}
 else if(hit==TOUCH_CANCEL||hit==TOUCH_CLOSE)*pressed=2;
 else if(hit==TOUCH_DEFAULT)*pressed=8;
}

static bool menu_was_down;
static bool menu_pointer_owned;
static int menu_capture;
static bool navigation_stick_centered;
/* Some CLOUD touch layouts provide a left stick while their D-pad only
   reaches the game's native menus. A fresh tilt moves one visible row. */
static unsigned touch_stick_navigation(void){
 float x=0,y=0;
 forge_input_getStickL(&x,&y);
 float ax=x<0?-x:x,ay=y<0?-y:y;
 if(ax<.35f&&ay<.35f)navigation_stick_centered=true;
 if(!navigation_stick_centered||(ax<.65f&&ay<.65f))return 0;
 navigation_stick_centered=false;
 unsigned key=ax>ay?(x<0?12:14):(y>0?13:15);
 return 1u<<key;
}
static void cancel_menu_touch(void){
 menu_was_down=true;menu_capture=TOUCH_NONE;
 if(position_editing&&(position_dragging||scale_dragging)){
  draft_x=drag_start_x;draft_y=drag_start_y;draft_percent=drag_start_percent;
 }
 position_dragging=scale_dragging=false;position_touch_down=true;
}
static bool settings_touch_input(unsigned *pressed){
 float x=0,y=0;
 bool down=wheel_touch_get(&x,&y);
 if(!wheel.editing){menu_capture=TOUCH_NONE;menu_pointer_owned=false;}
 else if(down){
  int hit=menu_touch_hit(x,y);
  if(!menu_was_down){
   menu_capture=hit;
   menu_pointer_owned=hit!=TOUCH_NONE ||
       (position_editing&&(scale_track_hit(x,y)||hit_circle(x,y,(TouchPoint){draft_x,draft_y},position_drag_radius())));
  }
  else if(hit!=menu_capture)menu_capture=TOUCH_NONE;
 }
 /* A touch may also be reported as an emulated pad press. Keep one owner
    through its release, including drags that leave the original control. */
 bool owned=menu_pointer_owned;
 if(owned)*pressed=0;
 if(!down){
  if(menu_was_down&&menu_capture){
   bool same_target=!bridge_area||host_modal||
       menu_touch_hit(bridge_pointer_x,bridge_pointer_y)==menu_capture;
   if(same_target)apply_menu_tap(menu_capture,pressed);
  }
  menu_capture=TOUCH_NONE;
  menu_pointer_owned=false;
 }
 menu_was_down=down;
 return owned;
}
