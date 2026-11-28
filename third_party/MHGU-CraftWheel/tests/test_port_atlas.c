/* Model the port's non-dereferenceable texture handles and separate upload
   buffers. Exercise the wheel's production path on ARM, including failure. */
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include <stdint.h>
#include "wheel_pages.inc"
enum { page_count=sizeof(wheel_pages)/sizeof(*wheel_pages) };
static void *atlas_ctx;
static ImTextureData *page_textures[page_count];
static double pc_time,page_retry_at[page_count];
static ImVec2_c v(float x,float y){return (ImVec2_c){x,y};}
static ImU32 pc_color(ImU32 c){return c;}
#include "port_atlas.inc"

/* Handles live outside the ARM test process's mapped range. */
static unsigned allocated,destroyed,registered,drawn,failed,force_failure;
static unsigned width,height,ready_handles[page_count+3];
static unsigned stage[512*248];
static const WheelSprite *current;
static unsigned handle_id(ImTextureData *tex){return (uintptr_t)tex-0xe0000000;}
ImTextureData *ImTextureData_ImTextureData(void){
    return (ImTextureData*)(uintptr_t)(0xe0000000 + ++allocated);
}
void ImTextureData_Create(ImTextureData *tex,ImTextureFormat format,int w,int h){
    if(handle_id(tex)!=allocated || format!=ImTextureFormat_RGBA32 ||
       w!=wheel_pages[current->page].width || h!=wheel_pages[current->page].height ||
       w<=0 || h<=0 || (unsigned)w*h>sizeof(stage)/sizeof(*stage))failed=1;
    width=w;height=h;
}
void *ImTextureData_GetPixels(ImTextureData *tex){
    if(handle_id(tex)!=allocated)failed=2;
    return force_failure?0:stage;
}
void ImTextureData_destroy(ImTextureData *tex){
    if(handle_id(tex)!=allocated || ready_handles[allocated])failed=3;
    ++destroyed;
}
void igRegisterUserTexture(ImTextureData *tex){
    unsigned i=handle_id(tex);
    const AtlasPage *p=&wheel_pages[current->page];
    if(i!=allocated || ready_handles[i] || width!=p->width || height!=p->height)failed=4;
    for(unsigned j=0;j<width*height;j++)if(stage[j]!=p->pixels[j])failed=5;
    ready_handles[i]=current->page+1;
    ++registered;
}
ImTextureRef_c ImTextureData_GetTexRef(ImTextureData *tex){
    return (ImTextureRef_c){tex,ImTextureID_Invalid};
}
void ImDrawList_AddImage(ImDrawList *d,ImTextureRef_c ref,ImVec2_c lo,ImVec2_c hi,
                         ImVec2_c uv,ImVec2_c uv1,ImU32 tint){
    (void)d;
    const WheelSprite *s=current;
    const AtlasPage *p=&wheel_pages[s->page];
    if(ready_handles[handle_id(ref._TexData)]!=s->page+1 || tint!=0xabcdef01 ||
       lo.x!=11+s->left || lo.y!=19+s->top || hi.x!=lo.x+s->w || hi.y!=lo.y+s->h ||
       (unsigned)(uv.x*p->width+.5f)!=s->x || (unsigned)(uv.y*p->height+.5f)!=s->y ||
       (unsigned)(uv1.x*p->width+.5f)!=s->x+s->w || (unsigned)(uv1.y*p->height+.5f)!=s->y+s->h)failed=6;
    ++drawn;
}
static void draw(const WheelSprite *sprite,unsigned edge){
    current=sprite;
    draw_packed_sprite(0,sprite,11,19,edge,edge,0xabcdef01);
}
static void draw_all(void){
    for(unsigned i=0;i<54;i++)for(unsigned lv=0;lv<4;lv++)draw(&control_sprites[i][lv],16u<<lv);
    for(unsigned i=0;i<100;i++)draw(&item_sprites[i],96);
    draw(&prowler_frame_normal,256);
    draw(&prowler_frame_selected,380);
    for(unsigned i=0;i<100;i++)if(prowler_icons[i].w)draw(&prowler_icons[i],96);
}
unsigned run_test(void){
    draw_all();
    if(allocated||drawn)return 11;
    atlas_ctx=(void*)1;
    force_failure=1;
    draw(&control_sprites[0][0],16);
    draw(&control_sprites[0][0],16);
    if(allocated!=1||destroyed!=1||drawn)return 12;
    pc_time=10000001;
    force_failure=0;
    draw_all();
    unsigned count=drawn;
    if(failed||!count||registered!=page_count||allocated!=1+page_count)return 20+failed;
    draw_all();
    return !failed && drawn==2*count && registered==page_count && allocated==page_count+1?0:30+failed;
}
