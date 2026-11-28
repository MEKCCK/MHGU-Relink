/* The same Forge HUD coordinates are used for menu hit testing and dragging. */
static int settings_hit(float x,float y){
 if(x>=48 && x<252 && y>=420 && y<480)return -2;
 if(x>=48 && x<252 && y>=484 && y<544)return -3;
 if(preset_row==5 && x>=303 && x<973 && y>=205 && y<425){
  unsigned row=(unsigned)((y-205)/55);
  return y<205+55*row+49?(int)row:-1;
 }
 if(preset_row!=4 || x<303 || x>=973 || y<194 || y>=194+11*33)return -1;
 unsigned row=(unsigned)((y-194)/33);
 return row==4 && !quick_enabled ? -1 : (int)row;
}
static void settings_touch_input(bool active,unsigned *pressed){
 static bool down_last;
 static int first=-1;
 float x=0,y=0;
 bool down=active?wheel_touch_get(&x,&y):forge_input_getTouch(&x,&y);
 if(!active || *pressed)first=-1;
 else if(down){
  int hit=settings_hit(x,y);
  if(!down_last)first=hit;
  else if(hit!=first)first=-1;
 }else if(down_last){
  if(first==-2||first==-3){
   previewing=false;
   preset_row=first==-3?5:4;
   if(preset_row==5)status="";
   detail_index=0;
   ui_focus=1;
  }else if(first>=0){
   ui_focus=1;
   detail_index=(unsigned)first;
   *pressed=1; /* Let the selected setting handle the normal A action. */
  }
  first=-1;
 }
 down_last=down;
}
