/* Pass vertices by value to avoid the guest point-array bridge. */
static void fill_convex(ImDrawList*d,const ImVec2_c*points,unsigned count,ImU32 color){
 if(count<3)return;
 for(unsigned i=1;i+1<count;i++)
  ImDrawList_AddTriangleFilled(d,points[0],points[i],points[i+1],color);
}
static void polygon(ImDrawList*d,float x,float y,const float*xy,unsigned n,float r,ImU32 c){
 ImVec2_c pts[8];for(unsigned i=0;i<n;i++)pts[i]=v(x+xy[i*2]*r,y+xy[i*2+1]*r);
 fill_convex(d,pts,n,pc_color(c));
}
static void line(ImDrawList*d,float x,float y,float xx,float yy,ImU32 c,float width){ImDrawList_AddLine(d,v(x,y),v(xx,yy),pc_color(c),width*scale);}
static const float hx[]={1,.41421356f,.41421356f,1,-.41421356f,1,-1,.41421356f,-1,-.41421356f,-.41421356f,-1,.41421356f,-1,1,-.41421356f};
static const float diamond[]={0,-1,1,0,0,1,-1,0};
static void selected_frame(ImDrawList*d,float x,float y,float r){
 polygon(d,x,y,diamond,4,r*1.58f,0xff000000);
 polygon(d,x,y,diamond,4,r*1.50f,0xff02baf3);
 polygon(d,x,y,diamond,4,r*1.43f,0xff02baf3);
 polygon(d,x,y,diamond,4,r*1.33f,0xff23333e);
}
static void background_diamond(ImDrawList*d,float x,float y,float r,float size,ImU32 col){
 // Clip every band against the item frame, so the texture cannot cover its rim.
 ImVec2_c a[16],b[16];unsigned n=4;
 for(unsigned i=0;i<4;i++)a[i]=(ImVec2_c){diamond[i*2]*size,diamond[i*2+1]*size};
 for(unsigned edge=0;edge<8&&n;edge++){
  unsigned next=(edge+1)%8,out=0;float ax=hx[edge*2]*r,ay=hx[edge*2+1]*r;
  float dx=(hx[next*2]-hx[edge*2])*r,dy=(hx[next*2+1]-hx[edge*2+1])*r;
  for(unsigned i=0;i<n;i++){
   ImVec2_c p=a[i],q=a[(i+1)%n];float fp=dx*(p.y-ay)-dy*(p.x-ax),fq=dx*(q.y-ay)-dy*(q.x-ax);
   if(fp>=0){if(out>=16)return;b[out++]=p;}
   if((fp>=0)!=(fq>=0)){if(out>=16)return;float t=fp/(fp-fq);b[out++]=(ImVec2_c){p.x+(q.x-p.x)*t,p.y+(q.y-p.y)*t};}
  }
  n=out;for(unsigned i=0;i<n;i++)a[i]=b[i];
 }
 for(unsigned i=0;i<n;i++)a[i]=v(x+a[i].x,y+a[i].y);
 if(n>=3)fill_convex(d,a,n,pc_color(col));
}
static void item_background(ImDrawList*d,float x,float y,float r){
 polygon(d,x,y,hx,8,r,0xff342818);
 for(unsigned i=0;i<3;i++){
  float size=r*(1.45f-i*.48f);
  background_diamond(d,x,y,r,size,0xff504331);
  background_diamond(d,x,y,r,size-r*.16f,0xff342818);
 }
}
static void unavailable_cross(ImDrawList*d,float x,float y){
   ImDrawList_PushClipRect(d,v(x-16,y-18),v(x+16,y+14),true);
   line(d,x-16,y-18,x+16,y+14,0xff000000,4);line(d,x+16,y-18,x-16,y+14,0xff000000,4);
   line(d,x-16,y-18,x+16,y+14,0xff3737f3,2);line(d,x+16,y-18,x-16,y+14,0xff3737f3,2);
   ImDrawList_PopClipRect(d);
  }
static void badge(ImDrawList*d,float x,float y,unsigned dir,Slot*p){
 bool mission=position_editing||(quest&&!wheel.editing),selected=dir==wheel.direction&&(!mission||!wheel_centered);
 bool cat=position_editing?cat_position_edit:mission&&playing_prowler;
 unsigned id=cat?cat_actions[dir]:(!mission&&previewing?preview_targets:wheel.targets)[dir];
 if(cat){
  /* The stock HUD uses a round panel at rest, and a larger panel
     with a separate gold rim when selected. Both sprites use center anchors. */
  const WheelSprite *frame=selected?&prowler_frame_selected:&prowler_frame_normal;
  float pixel_scale=48.f/256.f;
  draw_packed_sprite(d,frame,x-frame->w*pixel_scale*.5f,y-frame->h*pixel_scale*.5f,
                     frame->w*pixel_scale,frame->w,0xffffffff);
  if(id){
   draw_prowler_action(d,x-16,y-18,prowler_display_action(id));
   if(cat_low_gauge&(1u<<dir))unavailable_cross(d,x,y);
  }
  return;
 }
 unsigned flags=mission?item_state(id,p):0;
 ImU32 border=selected?(flags&2?0xff3939ed:flags&1?0xff55c45a:0xff55d5ff):0xff929b98;
 float r=mission?23:32;
 if(selected)selected_frame(d,x,y,r);
 polygon(d,x,y,hx,8,r+3,0xff000000);polygon(d,x,y,hx,8,r+1,0xffc3c9c6);
 polygon(d,x,y,hx,8,r,border);polygon(d,x,y,hx,8,r-2,0xff4a4e49);
 polygon(d,x,y,hx,8,r-3,0xff929b98);polygon(d,x,y,hx,8,r-4,0xff342818);
 item_background(d,x,y,r-4);
 if(id&&id<2991){
  item_icon(d,x-16,y-18,id);
  if(flags&4)unavailable_cross(d,x,y);
  unsigned qty=p?count(p,id):0;
  char n[12];number(n,qty);outlined_text(d,x+12,y+12,n,0xffeeeeee,18,1.5f);
 }
}
static void arrow(ImDrawList*d,float cx,float cy,float dx,float dy,float end,bool selected){
 float begin=20,half=selected?6:3;ImVec2_c points[4];
 float middle=begin+(end-begin)*.58f;
 points[0]=v(cx+dx*begin,cy+dy*begin);
 points[1]=v(cx+dx*middle-dy*half,cy+dy*middle+dx*half);
 points[2]=v(cx+dx*end,cy+dy*end);
 points[3]=v(cx+dx*middle+dy*half,cy+dy*middle-dx*half);
 fill_convex(d,points,4,pc_color(selected?0xff55d5ff:0xff666b62));
 for(unsigned i=0;i<4;i++)ImDrawList_AddLine(d,points[i],points[(i+1)%4],pc_color(0xff000000),1.5f*scale);
 if(selected)line(d,cx+dx*(begin+4),cy+dy*(begin+4),cx+dx*(end-3),cy+dy*(end-3),0xffc3f4ff,2);
}
static void center_stick(ImDrawList*d,float x,float y){atlas_icon(d,x-17,y-17,17,34);}
static void panel(ImDrawList*d,float x,float y,float w,float h){
 rect(d,x-3,y-3,w+6,h+6,0xff000000);rect(d,x,y,w,h,0xff727f80);rect(d,x+2,y+2,w-4,h-4,0xf0222023);
 line(d,x+4,y+4,x+w-4,y+4,0xffb2bdbd,1);
}
static void toggle(ImDrawList*d,float x,float y,bool on,bool disabled){
 rect(d,x,y,54,23,0xff000000);rect(d,x+2,y+2,50,19,disabled?0xff444444:on?0xff5a8a65:0xff656565);
 rect(d,x+(on?33:4),y+4,17,15,disabled?0xff777777:0xffeeeeee);
}
static void trigger_switch(ImDrawList*d,float x,float y){
 rect(d,x,y,66,23,0xff000000);rect(d,x+2,y+2,62,19,0xff535353);
 for(unsigned i=0;i<3;i++)rect(d,x+5+i*21,y+9,14,5,0xff292929);
 rect(d,x+4+release_trigger*21,y+4,16,15,0xffe3e8ed);
}
#include "prowler_menu.inc"
static void details(ImDrawList*d){
 text(d,310,164,"详细设置",0xff55d5ff,english?18:23);
 static const char*labels[]={"轮盘模式","调整轮盘位置","语言/Language","快捷切换","设定快捷切换键","触发方式","查看快捷键","说明","不透明度","设置按键图标风格","自定义呼出轮盘按键"};
 for(unsigned i=0;i<11;i++){
  float y=198+i*33;bool disabled=i==4&&!quick_enabled,selected=ui_focus&&i==detail_index;
  if(selected){rect(d,303,y-4,670,38,0xff55d5ff);rect(d,306,y-1,664,32,0xff3d3731);}
  ImU32 col=disabled?0xff777777:0xffeeeeee;text(d,318,y+3,labels[i],col,18);
  if(i==0){text(d,515,y+3,use_mode?"调和+使用道具":"仅调和",col,17);toggle(d,894,y+1,use_mode,false);}
  if(i==1)button(d,896,y,0,25);
  if(i==9)text(d,690,y+3,(const char*[]){"Xbox","Nintendo","PS"}[icon_style],col,17); if(i==8)text(d,600,y+3,(const char*[]){"25%","50%","75%","100%"}[opacity],col,17); if(i==2)text(d,600,y+3,english?"English":"中文",col,17);
  if(i==3){text(d,500,y+5,"游戏中可以按[OPEN]+自定义键切换预设",0xffcccccc,english?12:13);toggle(d,894,y+1,quick_enabled,false);}
  if(i==10)button(d,896,y,open_key,25);
  if(i==4){dim_button=disabled;button(d,896,y,quick_key,25);dim_button=false;}
  if(i==5){text(d,515,y+3,release_trigger==2?"按下右摇杆触发":release_trigger?"松开摇杆触发":"推动摇杆触发",col,17);trigger_switch(d,882,y+1);}
 }
 text(d,318,567,ui_focus?"[UP][DOWN] 选择  [A] 交互  [LEFT] 返回左栏  [B] 关闭":"[RIGHT] 进入详细设置  [B] 关闭",0xffeeeeee,16);
}
static void overlay(ImDrawList*d){
 if(info_open){
  panel(d,250,175,780,370);text(d,278,198,"说明",0xff55d5ff,24);
  text(d,278,250,"道具选择框为红色表示调和素材不足，",0xffeeeeee,19);
  text(d,278,282,"选择框为绿色表示达到携带上限，",0xffeeeeee,19);
  text(d,278,314,"有×表示未携带该道具。",0xffeeeeee,19);
  text(d,278,374,"松开摇杆触发、按下摇杆触发仅影响道具使用，",0xffeeeeee,19);
  text(d,278,406,"推动摇杆依旧可以连续调和。",0xffeeeeee,19);
  text(d,278,493,"[B] 返回",0xffeeeeee,19);
 }
 if(help_open){
  panel(d,300,180,680,340);text(d,328,202,"查看快捷键",0xff55d5ff,24);
  text(d,328,255,"配置快捷轮盘",0xffeeeeee,20);text(d,690,253,"[+]  3 / 3",0xffeeeeee,23);
  text(d,328,310,english?"Hold wheel key + move mouse to select":"长按呼出键，移动鼠标选择方向",0xffeeeeee,18);
  text(d,328,375,"快捷切换键",0xffeeeeee,20);button(d,690,374,open_key,26);text(d,721,374,"+",0xffeeeeee,22);button(d,743,374,quick_key,26);
  text(d,798,380,quick_enabled?"已启用":"未启用",quick_enabled?0xff55c45a:0xff999999,16);
  text(d,328,421,"长按呼出轮盘",0xffeeeeee,20);button(d,690,420,open_key,26);
  text(d,328,467,"[B] 返回",0xffeeeeee,19);

 }
 if(mapping){
  panel(d,280,250,720,180);
  text(d,308,275,mapping_open_key?"自定义呼出轮盘按键":"设定快捷切换键",0xff55d5ff,23);
  text(d,308,328,mapping_error?mapping_error:mapping_wait?"请先松开所有按键":"等待一个按键输入……",0xffeeeeee,20);
  text(d,308,382,mapping_open_key?"[+] 取消    [R3] 用于使用道具":"[L3] 取消    [OPEN]、[R3] 为保留键",0xffcccccc,16);
 }
}
static bool rendered_mission,collapsing;
static unsigned collapse_started;

void forge_onImGuiFreeRender(void){
 if(!ready){return;}
 if(!frequency||!fit_display())return;
 unsigned render_now=(unsigned)((nnosGetSystemTick()-start_tick)*1000/frequency);
 if(!quest||wheel.editing||(playing_prowler&&!cat_enabled)){collapsing=false;rendered_mission=false;}
 if(wheel.visible){collapsing=false;rendered_mission=quest&&!wheel.editing;}
 else{
  if(rendered_mission){rendered_mission=false;collapsing=true;collapse_started=render_now;}
  if(!collapsing)return;
  if((unsigned)(render_now-collapse_started)>=100){collapsing=false;return;}
 }
 bool mission=position_editing||(quest&&!wheel.editing);ImDrawList*d=igGetForegroundDrawList_Nil();if(!d)return; pc_alpha=mission&&!position_editing;
 Slot*p=pouch();float cx=position_editing?draft_x:mission?(playing_prowler?cat_x:position_x):640,cy=position_editing?draft_y:mission?(playing_prowler?cat_y:position_y):345;
 if(!mission){
  panel(d,280,100,720,520);text(d,310,116,"快捷轮盘设置",0xff55c7ea,25);text(d,610,124,"@Bilibili仲冬临沧的十一月",0xffcccccc,14);
  panel(d,40,100,220,520);text(d,60,116,"预设列表",0xff55c7ea,23);
  static const char*slots[]={"预设 1","预设 2","预设 3","预设 4","详细设置","猎猫设置"};
  for(unsigned i=0;i<6;i++){
   float y=166+i*64;bool selected=i==preset_row;
   rect(d,48,y-2,204,60,0xff000000);rect(d,50,y,200,56,selected?0xff55d5ff:0xff777777);rect(d,53,y+3,194,50,selected&&!ui_focus?0xff284757:0xe520201d);
   text(d,66,y+8,slots[i],selected?0xff55d5ff:0xffeeeeee,english?17:20);
   if(i<4)text(d,66,y+33,i==presets.active?"当前使用":(presets.valid&(1u<<i))?"已保存":"未保存",0xffcccccc,14);
   else text(d,66,y+33,"[RIGHT] 进入",0xffcccccc,14);
  }
  text(d,60,568,"[UP][DOWN] 选择",0xffeeeeee,16);
  if(preset_row==5){cat_settings_panel(d);text(d,310,594,status,0xff55c7ea,14);return;}
  if(preset_row==4){details(d);text(d,310,594,status,0xff55c7ea,14);overlay(d);return;}
  text(d,310,165,slots[preset_row],0xff55d5ff,20);
  text(d,550,169,"[LEFT] 保存  [RIGHT] 呼出",0xffcccccc,16);
  text(d,310,562,"[RS] 选择  [A] 登录  [L] 清除  [B] 返回",0xffeeeeee,english?16:19);
  text(d,310,594,status,0xff55c7ea,14);
 }
 float saved_scale=scale,saved_x=origin_x,saved_y=origin_y;
 if(mission&&!position_editing){
  unsigned now=(unsigned)((nnosGetSystemTick()-start_tick)*1000/frequency);
  float amount=wheel_open_progress(now);
  if(collapsing){float t=(unsigned)(render_now-collapse_started)/100.0f;amount=1-t*t*(3-2*t);}
  if(amount<=0)return;
  scale*=amount;origin_x+=cx*(saved_scale-scale);origin_y+=cy*(saved_scale-scale);
 }
 float spacing=mission?75:110,radius=mission?23:32;
 static const float dx[]={0,1,0,-1,.70710678f,.70710678f,-.70710678f,-.70710678f};
 static const float dy[]={-1,0,1,0,-.70710678f,.70710678f,.70710678f,-.70710678f};
 bool cat=position_editing?cat_position_edit:mission&&playing_prowler;
 if(cat){
  ImVec2_c center=v(cx,cy);
  // AddCircle insets its stroke path by half a screen pixel.
  float ring_radius=spacing*scale+.5f;
  ImDrawList_AddCircle(d,center,ring_radius,pc_color(0xff000000),64,6*scale);
  ImDrawList_AddCircle(d,center,ring_radius,pc_color(0xff5e655d),64,2*scale);
 }else{
  float frame=spacing+radius+7;
  for(unsigned i=0;i<4;i++){
   unsigned j=(i+1)%4;float x=cx+diamond[i*2]*frame,y=cy+diamond[i*2+1]*frame,xx=cx+diamond[j*2]*frame,yy=cy+diamond[j*2+1]*frame;
   line(d,x,y,xx,yy,0xff000000,6);line(d,x,y,xx,yy,0xff5e655d,2);
  }
 }
 for(unsigned i=0;i<8;i++)badge(d,cx+dx[i]*spacing,cy+dy[i]*spacing,i,p);
 // Draw arrows after ornaments.
 for(unsigned i=0;i<8;i++){
  bool selected=i==wheel.direction&&(!mission||!wheel_centered);
  arrow(d,cx,cy,dx[i],dy[i],selected?spacing-radius-3:spacing*.48f,selected);
 }
 center_stick(d,cx,cy);
 unsigned target=cat?cat_actions[wheel.direction]:(!mission&&previewing?preview_targets:wheel.targets)[wheel.direction];
 const char*name="";
 if(target&&!(mission&&wheel_centered))
  name=cat?prowler_action_name(prowler_display_action(target),english):target<2991?item_name(target):"";
 float size=mission?14:(english?18:20);float width=text_width(name,size);text(d,cx-width/2,cy+(mission?29:170),name,0xffeeeeee,size);
 scale=saved_scale;origin_x=saved_x;origin_y=saved_y;
 if(position_editing){
  float y=draft_y>360?18:633;
  panel(d,280,y,720,67);
  text(d,298,y+7,position_save_failed?"位置保存失败，按[A]重试或[B]撤销":"左摇杆移动，或触屏拖动轮盘",0xff55d5ff,17);
  text(d,298,y+37,"[A] 保存  [B] 撤销  [Y] 恢复默认",0xffeeeeee,17);
  return;
 }
 if(wheel.picker){
  panel(d,370,115,540,510);text(d,390,122,"从道具袋选择目标道具",0xff55c7ea,english?20:23);unsigned page=wheel.pick_index/16;
  for(unsigned n=0;n<16;n++){unsigned i=page*16+n;if(i>=40)break;float y=166+n*25;if(i==wheel.pick_index)rect(d,378,y-3,524,25,0xff009fb5);Slot s=p?p[i]:(Slot){0};text(d,392,y,s.id&&s.id<2991?item_name(s.id):"----",0xffeeeeee,18);if(s.id){char z[12];number(z,s.qty);text(d,850,y,z,0xffeeeeee,18);}}
  static const char*pages[]={"第 1 / 3 页","第 2 / 3 页","第 3 / 3 页"};text(d,550,563,pages[page],0xff55c7ea,16);
  text(d,390,590,"[UP][DOWN] 选择  [LEFT][RIGHT] 翻页  [A] 登录  [B] 返回",0xff55c7ea,english?13:15);
 }
 overlay(d);
pc_alpha=false;}


