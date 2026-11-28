static void touch_label(ImDrawList*d,float x,float y,float w,float h,const char*label,bool focused){
 rect(d,x,y,w,h,focused?0xff55d5ff:0xff929b98);
 rect(d,x+2,y+2,w-4,h-4,focused?0xff284757:0xff332f28);
 float size=english?16:18;
 text(d,x+(w-text_width(translate(label),size))*.5f,y+(h-size)*.5f,label,0xffeeeeee,size);
}
static void mode_switch(ImDrawList*d){
 bool focused=ui_focus&&detail_index==11;
 rect(d,690,149,283,44,focused?0xff55d5ff:0xff929b98);
 rect(d,692,151,279,40,focused?0xff284757:0xff332f28);
 text(d,702,160,"切换操作模式",0xffeeeeee,english?15:17);
 text(d,851,160,touch_mode?"触屏模式":"手柄模式",0xff55d5ff,english?15:17);
}
static void touch_details(ImDrawList*d){
 static const char*labels[]={"轮盘模式","调整轮盘位置","语言/Language","触发方式","说明","不透明度"};
 for(unsigned i=0;i<6;i++){
  float y=205+55*i;
  bool selected=ui_focus&&detail_index==touch_detail_rows[i];
  rect(d,303,y,670,49,selected?0xff55d5ff:0xff777777);
  rect(d,305,y+2,666,45,selected?0xff284757:0xff322f29);
  text(d,322,y+12,labels[i],0xffeeeeee,19);
  if(i==0)text(d,673,y+12,use_mode?"调和+使用道具":"仅调和",0xffeeeeee,18);
  if(i==2)text(d,673,y+12,english?"English":"中文",0xffeeeeee,18);
  if(i==3)text(d,673,y+12,touch_trigger_names[release_trigger],0xffeeeeee,18);
  if(i==5)text(d,673,y+12,(const char*[]){"25%","50%","75%","100%"}[opacity],0xffeeeeee,18);
 }
 if(!host_modal&&!bridge_area)
  text(d,310,558,"点移植版齿轮后可直接点选。左摇杆或方向键选择，[A]确认，[B]返回",0xffeeeeee,english?13:15);
}
static void position_size_slider(ImDrawList*d){
 float y=scale_track_y(), knob=SCALE_TRACK_LEFT+(SCALE_TRACK_RIGHT-SCALE_TRACK_LEFT)*(draft_percent-50)/100.0f;
 panel(d,275,y-25,724,50);
 text(d,291,y-12,"轮盘大小",0xffeeeeee,18);
 text(d,392,y-10,"[LEFT] [RIGHT]",0xffcfcec9,15);
 rect(d,SCALE_TRACK_LEFT,y-3,SCALE_TRACK_RIGHT-SCALE_TRACK_LEFT,6,0xff73716e);
 rect(d,SCALE_TRACK_LEFT,y-3,knob-SCALE_TRACK_LEFT,6,0xff55c7ea);
 rect(d,knob-7,y-13,14,26,0xff151412);
 rect(d,knob-5,y-11,10,22,0xff55d5ff);
 char label[12];number(label,draft_percent);
 unsigned n=draft_percent<100?2:3;label[n]='%';label[n+1]=0;
 text(d,921,y-12,label,0xffeeeeee,19);
}
static void touch_position_bar(ImDrawList*d,float y){
 panel(d,275,y,724,67);
 if(!touch_mode||(!host_modal&&!bridge_area)){
  text(d,291,y+9,position_save_failed?"位置保存失败":"左摇杆移动，或触屏拖动轮盘",0xff55d5ff,english?13:15);
  text(d,291,y+36,"[A] 保存  [B] 撤销  [Y] 恢复默认",0xffeeeeee,english?12:14);
 }else text(d,291,y+21,position_save_failed?"位置保存失败":"拖动轮盘选择位置",0xff55d5ff,english?16:19);
 touch_label(d,584,y+7,120,52,"保存",false);
 touch_label(d,718,y+7,120,52,"撤销",false);
 touch_label(d,852,y+7,137,52,"恢复默认",false);
}
static void touch_info(ImDrawList*d){
 panel(d,250,175,780,370);text(d,278,196,"说明",0xff55d5ff,24);
 static const char*lines[]={"[+] 第三页配置快捷轮盘，任务中长按背包展开轮盘。",
 "保持按住，向八个方向滑动选择道具。松手关闭轮盘。",
 "滑动触发时，滑向道具即可使用。保持方向可连续调和。",
 "松手或回中触发时，松手或滑回中心即可使用。",
 "滑至外圈确认时，继续向外滑至道具格即可使用。",
 "红框表示素材不足，绿框表示已达携带上限，× 表示未携带。"};
 for(unsigned i=0;i<6;i++)text(d,278,245+i*33,lines[i],0xffeeeeee,english?15:18);
 touch_label(d,820,481,150,45,"返回",false);
}
static void round_touch_key(ImDrawList*d,TouchPoint pt,bool confirm){
 float r=45;
 ImDrawList_AddRectFilled(d,v(pt.x-r,pt.y-r),v(pt.x+r,pt.y+r),0xff080808,r*scale,0);
 r-=3;ImDrawList_AddRectFilled(d,v(pt.x-r,pt.y-r),v(pt.x+r,pt.y+r),0xffb0b8b4,r*scale,0);
 r-=3;ImDrawList_AddRectFilled(d,v(pt.x-r,pt.y-r),v(pt.x+r,pt.y+r),0xff39342b,r*scale,0);
 if(confirm){
  line(d,pt.x-17,pt.y,pt.x-4,pt.y+13,0xffecf5e8,5);
  line(d,pt.x-4,pt.y+13,pt.x+19,pt.y-15,0xffecf5e8,5);
 }else{
  line(d,pt.x-13,pt.y-13,pt.x+13,pt.y+13,0xffecf5e8,5);
  line(d,pt.x-13,pt.y+13,pt.x+13,pt.y-13,0xffecf5e8,5);
 }
}
static void touch_picker_controls(ImDrawList*d){
 static const int dir[4][2]={{-1,0},{0,-1},{1,0},{0,1}};
 rect(d,170,332,56,56,0xff38342e);
 for(unsigned i=0;i<4;i++){
  float x=touch_pad_centers[i].x,y=touch_pad_centers[i].y;
  float dx=dir[i][0],dy=dir[i][1];
  panel(d,x-26,y-26,52,52);
  ImDrawList_AddTriangleFilled(d,v(x+dx*12,y+dy*12),v(x-dx*7+dy*10,y-dy*7-dx*10),
                               v(x-dx*7-dy*10,y-dy*7+dx*10),0xffecf5e8);
 }
 round_touch_key(d,confirm_center,true);round_touch_key(d,back_center,false);
}
