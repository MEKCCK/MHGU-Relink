#include <assert.h>
#include "wheel_cursor.c"
static bool captured,focused=true;
static int device=1,warps;
static int width=1920,height=1080;
static float mx,my;
static void (*work_item)(void *);
static void *argument;
static int input(void){return device;}
static void *window(void){return (void*)1;}
static void *focus(void){return focused?window():0;}
static bool relative(void *w){return captured;}
static bool logical(void *w,int *x,int *y){*x=width;*y=height;return true;}
static bool pixels(void *w,int *x,int *y){*x=width*2;*y=height*2;return true;}
static bool queue_work(void (*fn)(void *),void *arg,bool wait){work_item=fn;argument=arg;return true;}
static void warp(void *w,float x,float y){mx=x;my=y;++warps;}
static unsigned request(unsigned op,unsigned x,unsigned y){
 uint32_t regs[]={op,x,y};wheel_cursor(regs);return regs[0];
}
int main(void){
 last_device=input;game_window=window;keyboard_focus=focus;relative_mode=relative;
 window_size=logical;pixel_size=pixels;run_main=queue_work;warp_mouse=warp;
 captured=true;assert(request(1,1113,360));work_item(argument);assert(!warps);
 captured=false;assert(request(2,0,0));work_item(argument);assert(warps==1&&mx==1669.5f&&my==540);
 assert(!request(2,0,0));
 width=2560;height=1080;assert(request(1,700,400));work_item(argument);
 assert(warps==2&&mx==1370&&my==600);
 assert(request(1,300,500));request(0,0,0);work_item(argument);assert(warps==2);
 device=0;assert(!request(1,300,500));device=1;focused=false;
 assert(request(1,300,500));work_item(argument);assert(warps==2);
}
