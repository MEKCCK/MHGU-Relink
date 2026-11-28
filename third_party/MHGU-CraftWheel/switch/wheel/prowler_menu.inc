static void cat_settings_panel(ImDrawList *d){
 static const char *const labels[]={"猎猫轮盘","触发方式","调整轮盘位置","不透明度"};
 text(d,310,164,"猎猫设置",0xff55d5ff,english?18:23);
 for(unsigned i=0;i<4;i++){
  float y=205+55*i;
  bool selected=ui_focus&&detail_index==i;
  rect(d,303,y,670,49,selected?0xff55d5ff:0xff777777);
  rect(d,305,y+2,666,45,selected?0xff284757:0xff322f29);
  text(d,322,y+12,labels[i],0xffeeeeee,19);
  if(i==0)text(d,680,y+12,cat_enabled?"已启用":"未启用",0xffeeeeee,18);
  if(i==1)text(d,680,y+12,cat_trigger?"松开触发":"推动触发",0xffeeeeee,18);
  if(i==2)text(d,680,y+12,"调整",0xffeeeeee,18);
  if(i==3)text(d,680,y+12,(const char*[]){"25%","50%","75%","100%"}[cat_opacity],0xffeeeeee,18);
 }
 text(d,310,566,ui_focus?"[UP][DOWN] 选择  [A] 交互  [LEFT] 返回左栏  [B] 关闭":"[RIGHT] 进入详细设置  [B] 关闭",0xffeeeeee,16);
}
