/* Forge turns the host pointer into 1280x720 game HUD coordinates.  Its
   chosen HUD width and game aspect can differ from our fitted 16:9 canvas. */
static void wheel_touch_to_canvas(float*x,float*y,float window_w,float window_h,
                                 unsigned render_w,unsigned render_h,unsigned hud_w,unsigned hud_h){
 float picture_aspect=(float)render_w/render_h;
 float hud_aspect=(float)hud_w/hud_h;
 if(hud_aspect>picture_aspect)hud_aspect=picture_aspect;
 float picture_h=window_w/picture_aspect;
 if(picture_h>window_h)picture_h=window_h;
 float fit=window_w/1280.0f;
 if(window_h/720.0f<fit)fit=window_h/720.0f;
 *x=640.0f+(*x-640.0f)*((picture_h*hud_aspect)/(1280.0f*fit));
 *y=360.0f+(*y-360.0f)*(picture_h/(720.0f*fit));
}

static bool wheel_game_touch(float*x,float*y){
 if(!forge_input_getTouch(x,y))return false;
 if(!ready)return true;
 /* Build guard for the guest renderer constructor, relative to main. The
    original instructions publish the renderer to the global at GOT 1838db0.
    MHGURecomp tools/gen.py substitutes the selected render and HUD sizes
    here. This reads the guest's initialized fields, and imports no host API. */
 static int geometry_supported;
 if(!geometry_supported){
  geometry_supported=*(const uint32_t*)(base+0xbbcda8)==0xe59f09bc &&
                     *(const uint32_t*)(base+0xbbcdb4)==0xe79f0000 &&
                     *(const uint32_t*)(base+0xbbcdb8)==0xe5804000 &&
                     *(const uint32_t*)(base+0xbbd76c)==0xc7bff4 &&
                     *(const uint32_t*)(base+0xbbcd50)==0xe7a67000 ? 1 : -1;
 }
 if(geometry_supported<0)return true;
 const uint8_t*const*slot=*(const uint8_t*const*const*)(base+0x1838db0);
 const uint8_t*renderer=slot?*slot:0;
 if(!renderer)return true;
 /* The constructor's docked render pair is at +44bac0; its UI pair follows.
    Use the docked pair, even if the engine currently selects a Handheld mode. */
 const uint32_t*dims=(const uint32_t*)(renderer+0x44bac0);
 unsigned rw=dims[0],rh=dims[1],uw=dims[2],uh=dims[3];
 if(rw<160||rw>32768||rh<90||rh>32768||uw<1280||uw>32768||uh!=720)return true;
 ImGuiIO*io=igGetIO_Nil();
 if(!io || !(io->DisplaySize.x>=100 && io->DisplaySize.x<=32768 &&
             io->DisplaySize.y>=100 && io->DisplaySize.y<=32768))return true;
 wheel_touch_to_canvas(x,y,io->DisplaySize.x,io->DisplaySize.y,rw,rh,uw,uh);
 return true;
}
