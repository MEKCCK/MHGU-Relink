/* Store logical pixels on the same centered 1280 x 720 canvas as the renderer.
   Leave space for the selected frame and three-digit item quantities. */
enum {
 POSITION_DEFAULT_X = 1113, POSITION_DEFAULT_Y = 360,
 POSITION_MIN_X = 128, POSITION_MAX_X = 1280 - POSITION_MIN_X,
 POSITION_MIN_Y = 116, POSITION_MAX_Y = 720 - POSITION_MIN_Y,
 POSITION_MAGIC = 0x32505747
};
typedef struct {uint32_t magic,seq,x,y,crc;} WheelPositionDisk;
static unsigned position_x=POSITION_DEFAULT_X, position_y=POSITION_DEFAULT_Y, position_sequence;
static float draft_x,draft_y,drag_offset_x,drag_offset_y;
static bool position_editing,position_quest,position_touch_down,position_dragging,position_save_failed;
static unsigned position_last_ms;
extern void forge_input_getStickL(float*,float*);
extern bool forge_input_getTouch(float*,float*);
#include "wheel_touch.inc"

static bool read_position(const char*path,WheelPositionDisk*d){
 void*f=open_file(path,0x11);
 if(!f)return false;
 unsigned size=file_size(f);
 bool ok=false;
 if(size==sizeof(*d)){
  ok=file_read(f,d,sizeof(*d))==sizeof(*d)&&d->magic==POSITION_MAGIC&&
     d->x>=POSITION_MIN_X&&d->x<=POSITION_MAX_X&&d->y>=POSITION_MIN_Y&&d->y<=POSITION_MAX_Y&&
     crc(d,sizeof(*d)-4)==d->crc;
 }else if(size==sizeof(PositionDisk)){
  PositionDisk old;
  if(file_read(f,&old,size)==size&&old.magic==0x31505747&&old.left<=1&&crc(&old,size-4)==old.crc){
   // Preserve the actual center used by the old selected layout.
   *d=(WheelPositionDisk){POSITION_MAGIC,old.seq,old.left?167:1113,old.left?560:460,0};
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
 position_sequence=d?d->seq:0;
}
static bool save_position(unsigned x,unsigned y){
 WheelPositionDisk d={POSITION_MAGIC,position_sequence+1,x,y,0},check;
 d.crc=crc(&d,sizeof(d)-4);
 if(!storage_begin())return false;
 const char*path=position_paths[d.seq&1];
 void*f=open_file(path,0x13);
 bool ok=false;
 if(f){
  unsigned n=file_write(f,&d,sizeof(d));
  close_file(f);
  ok=n==sizeof(d)&&read_position(path,&check)&&check.seq==d.seq&&check.x==x&&check.y==y&&check.crc==d.crc;
 }
 storage_end();
 if(ok){position_x=x;position_y=y;position_sequence=d.seq;}
 return ok;
}

static unsigned position_time_ms(void){return (unsigned)((nnosGetSystemTick()-start_tick)*1000/frequency);}
static void begin_position_edit(void){
 cat_position_edit=preset_row==5;
 draft_x=(float)(cat_position_edit?cat_x:position_x);
 draft_y=(float)(cat_position_edit?cat_y:position_y);
 position_quest=quest;
 position_last_ms=position_time_ms();
 float tx,ty;
 position_touch_down=wheel_touch_get(&tx,&ty);
 position_dragging=false;
 position_save_failed=false;
 position_editing=true;
}
static float position_clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}

static void update_position_edit(unsigned held,unsigned pressed){
 unsigned now=position_time_ms(),ms=now-position_last_ms;
 position_last_ms=now;
 if(pressed&2){status="已撤销位置更改";position_editing=false;}
 else if(pressed&1){
  unsigned x=(unsigned)(draft_x+.5f), y=(unsigned)(draft_y+.5f);
  bool saved=cat_position_edit?save_cat_options(cat_enabled,cat_trigger,cat_opacity,x,y,100):
                               save_position(x,y);
  if(saved){
   status="轮盘位置已保存";position_editing=false;
  }else position_save_failed=true;
 }else if(pressed&8){
  draft_x=POSITION_DEFAULT_X;draft_y=POSITION_DEFAULT_Y;position_dragging=false;position_save_failed=false;
 }
 if(!position_editing){cat_position_edit=false;release_mask|=held;return;}
 float tx=0,ty=0;
 bool down=wheel_touch_get(&tx,&ty);
 if(!down)position_dragging=false;
 else if(!position_touch_down){
  drag_offset_x=draft_x-tx;drag_offset_y=draft_y-ty;
  position_dragging=drag_offset_x*drag_offset_x+drag_offset_y*drag_offset_y<=116*116;
 }
 position_touch_down=down;
 if(pressed&11)return;
 if(position_dragging){
  draft_x=tx+drag_offset_x;draft_y=ty+drag_offset_y;
 }else{
  float sx=0,sy=0;
  forge_input_getStickL(&sx,&sy);
  if(sx*sx+sy*sy>.20f*.20f){
   float step=(ms<100?ms:100)*.45f;
   draft_x+=sx*step;draft_y-=sy*step;
  }
 }
 draft_x=position_clamp(draft_x,POSITION_MIN_X,POSITION_MAX_X);
 draft_y=position_clamp(draft_y,POSITION_MIN_Y,POSITION_MAX_Y);
}
