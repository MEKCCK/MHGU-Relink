/* Store logical pixels on the same centered 1280 x 720 canvas as the renderer.
   Leave space for the selected frame and three-digit item quantities. */
enum {
 POSITION_DEFAULT_X = 1113, POSITION_DEFAULT_Y = 360,
 POSITION_MIN_X = 128, POSITION_MAX_X = 1280 - POSITION_MIN_X,
 POSITION_MIN_Y = 116, POSITION_MAX_Y = 720 - POSITION_MIN_Y,
 POSITION_MIN_PERCENT = 50, POSITION_MAX_PERCENT = 150, POSITION_DEFAULT_PERCENT = 100,
 POSITION_MAGIC = 0x33505747,
 SCALE_TRACK_LEFT = 505, SCALE_TRACK_RIGHT = 900
};
typedef struct {uint32_t magic,seq,x,y,crc;} OldWheelPositionDisk;
typedef struct {uint32_t magic,seq,x,y,percent,crc;} WheelPositionDisk;
static unsigned position_x=POSITION_DEFAULT_X, position_y=POSITION_DEFAULT_Y, position_sequence;
static unsigned position_percent=POSITION_DEFAULT_PERCENT,draft_percent,drag_start_percent;
static float draft_x,draft_y,drag_offset_x,drag_offset_y;
static float drag_start_x,drag_start_y;
static bool position_editing,position_quest,position_touch_down,position_dragging,scale_dragging,position_save_failed;
static unsigned position_last_ms,scale_repeat_at;
extern void forge_input_getStickL(float*,float*);
extern bool forge_input_getTouch(float*,float*);
#include "wheel_touch.inc"
static bool wheel_touch_get(float*x,float*y){
 if(bridge_area&&!host_modal){
  if(!bridge_pointer_down)return false;
  *x=bridge_pointer_x;*y=bridge_pointer_y;return true;
 }
 if(!host_modal)return wheel_game_touch(x,y);
 if(!host_pointer_down)return false;
 *x=host_pointer_x;*y=host_pointer_y;
 return true;
}

static unsigned position_margin(unsigned extent,unsigned percent){return (extent*percent+99)/100;}
static bool valid_center(unsigned x,unsigned y,unsigned percent){
 unsigned mx=position_margin(POSITION_MIN_X,percent),my=position_margin(POSITION_MIN_Y,percent);
 return x>=mx&&x<=1280-mx&&y>=my&&y<=720-my;
}
static bool read_position(const char*path,WheelPositionDisk*d){
 void*f=open_file(path,0x11);
 if(!f)return false;
 unsigned size=file_size(f);
 bool ok=false;
 if(size==sizeof(*d)){
  ok=file_read(f,d,sizeof(*d))==sizeof(*d)&&d->magic==POSITION_MAGIC&&
     d->percent>=POSITION_MIN_PERCENT&&d->percent<=POSITION_MAX_PERCENT&&valid_center(d->x,d->y,d->percent)&&
     crc(d,sizeof(*d)-4)==d->crc;
 }else if(size==sizeof(OldWheelPositionDisk)){
  OldWheelPositionDisk old;
  if(file_read(f,&old,size)==size&&old.magic==0x32505747&&valid_center(old.x,old.y,100)&&crc(&old,size-4)==old.crc){
   *d=(WheelPositionDisk){POSITION_MAGIC,old.seq,old.x,old.y,100,0};
   d->crc=crc(d,sizeof(*d)-4);
   ok=true;
  }
 }else if(size==sizeof(PositionDisk)){
  PositionDisk old;
  if(file_read(f,&old,size)==size&&old.magic==0x31505747&&old.left<=1&&crc(&old,size-4)==old.crc){
   // Preserve the actual center used by the old selected layout.
   *d=(WheelPositionDisk){POSITION_MAGIC,old.seq,old.left?167:1113,old.left?560:460,100,0};
   d->crc=crc(d,sizeof(*d)-4);
   ok=true;
  }
 }
 close_file(f);
 return ok;
}
static void load_position(void){
 WheelPositionDisk a,b;
 bool va=read_position(position_paths[0],&a),vb=read_position(position_paths[1],&b);
 WheelPositionDisk*d=vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:0;
 position_x=d?d->x:POSITION_DEFAULT_X;
 position_y=d?d->y:POSITION_DEFAULT_Y;
 position_percent=d?d->percent:POSITION_DEFAULT_PERCENT;
 position_sequence=d?d->seq:0;
}
static bool save_position(unsigned x,unsigned y,unsigned percent){
 WheelPositionDisk d={POSITION_MAGIC,position_sequence+1,x,y,percent,0},check;
 d.crc=crc(&d,sizeof(d)-4);
 if(!storage_begin())return false;
 const char*path=position_paths[d.seq&1];
 void*f=open_file(path,0x13);
 bool ok=false;
 if(f){
  unsigned n=file_write(f,&d,sizeof(d));
  close_file(f);
  ok=n==sizeof(d)&&read_position(path,&check)&&check.seq==d.seq&&check.x==x&&check.y==y&&check.percent==percent&&check.crc==d.crc;
 }
 storage_end();
 if(ok){position_x=x;position_y=y;position_percent=percent;position_sequence=d.seq;}
 return ok;
}

static unsigned position_time_ms(void){return (unsigned)((nnosGetSystemTick()-start_tick)*1000/frequency);}
static void begin_position_edit(void){
 cat_position_edit=preset_row==5;
 draft_x=(float)(cat_position_edit?cat_x:position_x);
 draft_y=(float)(cat_position_edit?cat_y:position_y);
 draft_percent=cat_position_edit?cat_scale:position_percent;
 position_quest=quest;
 position_last_ms=position_time_ms();
 float tx,ty;
 position_touch_down=wheel_touch_get(&tx,&ty);
 position_dragging=scale_dragging=false;
 scale_repeat_at=position_last_ms+350;
 position_save_failed=false;
 position_editing=true;
}
static float position_clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static float position_toolbar_y(void){return draft_y>360?18:633;}
static float scale_track_y(void){return draft_y>360?111:607;}
static bool scale_track_hit(float x,float y){
 float cy=scale_track_y();
 return x>=SCALE_TRACK_LEFT-14 && x<=SCALE_TRACK_RIGHT+14 && y>=cy-22 && y<=cy+22;
}
static float position_drag_radius(void){return 116.0f*draft_percent/100.0f;}
static void clamp_draft_center(void){
 unsigned mx=position_margin(POSITION_MIN_X,draft_percent),my=position_margin(POSITION_MIN_Y,draft_percent);
 draft_x=position_clamp(draft_x,mx,1280-mx);
 draft_y=position_clamp(draft_y,my,720-my);
}
static void move_position_pointer(float x,float y){
 if(scale_dragging){
  float amount=position_clamp((x-SCALE_TRACK_LEFT)/(SCALE_TRACK_RIGHT-SCALE_TRACK_LEFT),0,1);
  draft_percent=POSITION_MIN_PERCENT+(unsigned)(amount*(POSITION_MAX_PERCENT-POSITION_MIN_PERCENT)+.5f);
 }else if(position_dragging){
  draft_x=x+drag_offset_x;draft_y=y+drag_offset_y;
 }
 clamp_draft_center();
}

static void update_position_edit(unsigned held,unsigned pressed,bool allow_stick){
 unsigned now=position_time_ms(),ms=now-position_last_ms;
 position_last_ms=now;
 if(pressed&2){status="已撤销位置更改";position_editing=false;}
 else if(pressed&1){
  unsigned x=(unsigned)(draft_x+.5f), y=(unsigned)(draft_y+.5f);
  bool saved=cat_position_edit?save_cat_options(cat_enabled,cat_trigger,cat_opacity,x,y,draft_percent):
                               save_position(x,y,draft_percent);
  if(saved){
   status="轮盘位置已保存";position_editing=false;
  }else position_save_failed=true;
 }else if(pressed&8){
  draft_x=POSITION_DEFAULT_X;draft_y=POSITION_DEFAULT_Y;draft_percent=POSITION_DEFAULT_PERCENT;
  position_dragging=scale_dragging=false;position_save_failed=false;
 }
 if(!position_editing){cat_position_edit=false;release_mask|=held;return;}
 float tx=0,ty=0;
 bool down=wheel_touch_get(&tx,&ty);
 if(!down){
  /* The CLOUD bridge supplies the finger's lift coordinates, even when the
     host received no separate motion event immediately before the lift. */
  if(position_touch_down&&bridge_area&&!host_modal&&!bridge_pointer_cancelled&&(position_dragging||scale_dragging))
   move_position_pointer(bridge_pointer_x,bridge_pointer_y);
  position_dragging=scale_dragging=false;
 }else if(!position_touch_down){
  drag_offset_x=draft_x-tx;drag_offset_y=draft_y-ty;
  scale_dragging=scale_track_hit(tx,ty);
  float radius=position_drag_radius();
  position_dragging=!scale_dragging&&drag_offset_x*drag_offset_x+drag_offset_y*drag_offset_y<=radius*radius;
  drag_start_x=draft_x;drag_start_y=draft_y;drag_start_percent=draft_percent;
 }
 position_touch_down=down;
 if(pressed&11)return;
 if(position_dragging||scale_dragging){
  move_position_pointer(tx,ty);
 }else if(allow_stick){
  const unsigned left=1u<<12,right=1u<<14;
  unsigned key=held&(left|right);
  if((key==left||key==right)&&((pressed&key)||(int32_t)(now-scale_repeat_at)>=0)){
   int next=(int)draft_percent+(key==right?1:-1);
   draft_percent=(unsigned)position_clamp(next,POSITION_MIN_PERCENT,POSITION_MAX_PERCENT);
   scale_repeat_at=now+((pressed&key)?350:45);
  }
  float sx=0,sy=0;
  forge_input_getStickL(&sx,&sy);
  if(sx*sx+sy*sy>.20f*.20f){
   float step=(ms<100?ms:100)*.45f;
   draft_x+=sx*step;draft_y-=sy*step;
  }
 }
 clamp_draft_center();
}
