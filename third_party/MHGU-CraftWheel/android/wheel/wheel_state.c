#include <stdint.h>
#include <stdbool.h>
typedef struct {uint16_t id,qty;} Slot;
typedef struct {
 uint32_t visible,editing,picker,direction,pick_index,targets[8];
 uint32_t hold_since,holding,armed,latch,action,action_item;
} WheelState;
WheelState wheel;
static unsigned last_quest=2;
static unsigned l_waiting,l_started;
unsigned release_trigger;
unsigned playing_prowler,wheel_cat_trigger,wheel_cat_enabled;
uint32_t cat_actions[8];
static unsigned block_old_gesture;
void wheel_cancel_gesture(void){
 if(wheel.editing)return;
 block_old_gesture|=wheel.visible||wheel.holding||l_waiting;
 wheel.visible=wheel.holding=wheel.latch=wheel.action=wheel.action_item=0;
 l_waiting=0;
}
unsigned wheel_trigger_now(void){return playing_prowler?wheel_cat_trigger:release_trigger;}
unsigned open_key=6; /* Forge/Npad button index, L by default. */
unsigned preset_row,ui_focus,detail_index,quick_enabled;
bool touch_mode;
/* In display order, with the mode switch above the first detail row. */
const unsigned touch_detail_rows[7]={0,1,2,5,7,8,11};
static const unsigned controller_detail_rows[12]={0,1,2,3,4,5,6,7,8,9,10,11};
unsigned wheel_opened_at;
unsigned wheel_centered=1;
float wheel_open_progress(unsigned now){
 unsigned elapsed=now-wheel_opened_at;if(elapsed>=150)return 1;
 float t=elapsed/150.0f;return t*t*(3-2*t);
}
#define A 1u
#define B 2u
#define Y 8u
#define L 64u
#define R 128u
#define R3 32u
#define LEFT (1u<<12)
#define UP (1u<<13)
#define RIGHT (1u<<14)
#define DOWN (1u<<15)
/* Cardinal indices 0..3 retain the four-way configuration; diagonals 4..7
   are upper-right, lower-right, lower-left and upper-left. */
static int direction(int x,int y){
 if(x>32767)x=32767;if(x<-32767)x=-32767;
 if(y>32767)y=32767;if(y<-32767)y=-32767;
 int ax=x<0?-x:x,ay=y<0?-y:y;
 if(ax*ax+ay*ay<18000*18000)return -1;
 if(ax*1000<ay*414)return y>0?0:2;
 if(ay*1000<ax*414)return x>0?1:3;
 if(y>0)return x>0?4:7;
 return x>0?5:6;
}
void wheel_reset(void){uint32_t*p=(uint32_t*)&wheel;for(unsigned i=0;i<sizeof(wheel)/4;i++)p[i]=0;wheel.armed=1;last_quest=2;l_waiting=0;ui_focus=detail_index=preset_row=0;}
static unsigned next_slot(unsigned i,int step,const Slot*p){
 for(unsigned n=0;n<40;n++){i=(i+40+step)%40;if(p[i].id&&p[i].qty)return i;}return i;
}
static unsigned next_page(unsigned i,int step,const Slot*p){
 unsigned page=(i/16+3+step)%3,start=page*16,length=page==2?8:16,row=i%16;
 if(row>=length)row=length-1;
 for(unsigned n=0;n<length;n++){unsigned row_at=row+n;if(row_at>=length)row_at-=length;unsigned j=start+row_at;if(p[j].id&&p[j].qty)return j;}
 return start+row;
}
void wheel_enter_settings(unsigned in_quest){
 last_quest=in_quest;
 l_waiting=0;
 wheel.visible=wheel.editing=1;
 wheel.picker=wheel.holding=wheel.action=wheel.action_item=0;
 wheel.armed=0;
 wheel.latch=2;
}
void wheel_step(unsigned quest,unsigned held,unsigned pressed,int x,int y,unsigned now,const Slot*p){
 WheelState*s=&wheel;s->action=0;s->action_item=0;
 unsigned trigger=wheel_trigger_now();
 if(quest!=last_quest){s->visible=s->editing=s->picker=s->holding=s->latch=0;s->armed=1;last_quest=quest;l_waiting=0;}
 if(quest&&!s->editing&&trigger==2&&s->visible&&(held&(1u<<open_key))&&(pressed&R3)){
  int click_dir=direction(x,y);wheel_centered=click_dir<0;
  if(click_dir>=0)s->direction=click_dir;
  s->action=12;s->action_item=click_dir<0?0:s->targets[click_dir];return;
 }
 if(quest&&!s->editing){
  if(!(held&(1u<<open_key)))block_old_gesture=0;
  if(block_old_gesture||(playing_prowler&&!wheel_cat_enabled)){
   s->visible=s->holding=s->latch=l_waiting=0;return;
  }
  s->picker=0;
  if(!(held&(1u<<open_key))){s->visible=s->holding=0;s->latch=0;s->armed=1;l_waiting=0;return;}
  if(!s->visible){
   if(!l_waiting){l_waiting=1;l_started=now;}
   if((unsigned)(now-l_started)<200)return;
  }
  int d=direction(x,y);
  if(d>=0)wheel_centered=0;
  else if((int64_t)x*x+(int64_t)y*y<8000*8000)wheel_centered=1;
  if(!s->visible){s->visible=1;s->latch=d>=0?2:0;s->holding=0;wheel_opened_at=now;}
  if(d>=0){
   s->direction=(unsigned)d;
   unsigned first=!s->latch||(s->holding&&s->holding!=(unsigned)d+1);
   if(first||
      (s->latch==1&&s->holding&&(unsigned)(now-s->hold_since)>=150)){
    s->latch=1;s->holding=(unsigned)d+1;s->hold_since=now;
    if(playing_prowler){
     s->latch=2;
     if(!trigger){s->action=14;s->action_item=cat_actions[d];}
    }else{s->action=1;s->action_item=s->targets[d];}
   }
  }
  else{
   if((int64_t)x*x+(int64_t)y*y<8000*8000){
    if(trigger==1&&s->holding){
     s->direction=s->holding-1;
     s->action=playing_prowler?14:9;
     s->action_item=playing_prowler?cat_actions[s->direction]:s->targets[s->direction];
    }
    s->holding=0;s->latch=0;
   }
  }
  return;
 }
 if(!quest&&s->visible&&!s->editing)s->visible=0;
 if(!s->editing)return;
 l_waiting=0;
 if(pressed&B){if(s->picker)s->picker=0;else{s->editing=s->visible=0;}return;}
 if(s->picker){
  if(!p)return;
  if(pressed&LEFT)s->pick_index=next_page(s->pick_index,-1,p);
  else if(pressed&RIGHT)s->pick_index=next_page(s->pick_index,1,p);
  else if(pressed&UP)s->pick_index=next_slot(s->pick_index,-1,p);
  else if(pressed&DOWN)s->pick_index=next_slot(s->pick_index,1,p);
  if((pressed&A)&&p[s->pick_index].id&&p[s->pick_index].qty){
   s->targets[s->direction]=p[s->pick_index].id;s->picker=0;s->action=2;
  }
  return;
 }
 int d=direction(x,y);if(d>=0)s->direction=(unsigned)d;
 if(preset_row==5){
  if(ui_focus){
   if(pressed&LEFT)ui_focus=0;
   else if(pressed&(UP|DOWN))detail_index=(detail_index+((pressed&DOWN)?1:3))%4;
   else if(pressed&A){s->action=13;s->action_item=detail_index;}
  }else if(pressed&(UP|DOWN)){s->action=6;s->action_item=(pressed&DOWN)?1:0;}
  else if(pressed&(A|RIGHT)){ui_focus=1;detail_index=0;}
  return;
 }
 if(ui_focus){
  if(pressed&LEFT){ui_focus=0;return;}
  if(pressed&(UP|DOWN)){
   const unsigned*rows=touch_mode?touch_detail_rows:controller_detail_rows;
   unsigned count=touch_mode?7:12,row=0;
   while(row+1<count&&rows[row]!=detail_index)row++;
   do{row=(row+((pressed&DOWN)?1:count-1))%count;}while(rows[row]==4&&!quick_enabled);
   detail_index=rows[row];
   return;
  }
  if(pressed&A){s->action=11;s->action_item=detail_index;}return;
 }
 if((pressed&16)){s->action=10;return;}
 if(pressed&(UP|DOWN)){s->action=6;s->action_item=(pressed&DOWN)?1:0;return;}
 if(pressed&LEFT){s->action=7;return;}
 if(pressed&RIGHT){if(preset_row==4)ui_focus=1;else s->action=8;return;}
 if(preset_row==4){if(pressed&A)ui_focus=1;return;}
 if((pressed&L)){if(s->targets[s->direction]){s->targets[s->direction]=0;s->action=5;}return;}
 if((pressed&A)&&p){s->pick_index=next_slot(39,1,p);s->picker=1;}
}

