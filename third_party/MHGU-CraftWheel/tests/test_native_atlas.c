/* Exercise the actual texture/page helpers with the pinned native ABI. */
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include <string.h>
#include "wheel_pages.inc"
static ImVec2_c v(float x,float y){return (ImVec2_c){x,y};}
static ImU32 pc_color(ImU32 c){return c;}
#include "native_atlas.inc"

static unsigned registrations,unregistrations,context_id,draws,freed,used,failed;
static AtlasContext backing[4];
static ImGuiContextHook hooks[4];
static const WheelSprite *expected_sprite;
static unsigned expected_edge;
void *memset(void*d,int c,size_t n){unsigned char *p=d;while(n--)*p++=c;return d;}
ImGuiContext *igGetCurrentContext(void){return (ImGuiContext*)(0x900000+context_id*16);}
void *igMemAlloc(size_t n){return n==sizeof(AtlasContext)&&used<4?&backing[used++]:0;}
void igMemFree(void*p){if((AtlasContext*)p<backing||(AtlasContext*)p>=backing+4)failed++;else freed++;}
ImGuiID igAddContextHook(ImGuiContext*ctx,const ImGuiContextHook*h){
 if(ctx!=igGetCurrentContext()||h->Type!=ImGuiContextHookType_Shutdown)failed++;
 hooks[context_id]=*h;return 1;
}
void igRegisterUserTexture(ImTextureData*t){
 if(t->RefCount || !t->Pixels || t->BytesPerPixel!=4 || t->TexID!=ImTextureID_Invalid ||
    t->Status!=ImTextureStatus_WantCreate || t->BackendUserData)failed++;
 ++t->RefCount;++registrations;
}
void igUnregisterUserTexture(ImTextureData*t){
 if(t->RefCount!=1)failed++;
 --t->RefCount;++unregistrations;
}
ImTextureRef_c ImTextureData_GetTexRef(ImTextureData*t){return (ImTextureRef_c){t,ImTextureID_Invalid};}
void ImDrawList_AddImage(ImDrawList*d,ImTextureRef_c ref,ImVec2_c lo,ImVec2_c hi,ImVec2_c uv,ImVec2_c uv1,ImU32 tint){
 (void)d;
 const WheelSprite*s=expected_sprite;
 ImTextureData*t=ref._TexData;
 if(t->RefCount!=1 || (t->Status!=ImTextureStatus_WantCreate&&t->Status!=ImTextureStatus_OK) ||
    t->Pixels!=(unsigned char*)wheel_pages[s->page].pixels || tint!=0xabcdef01 ||
    lo.x!=11+s->left || lo.y!=19+s->top || hi.x!=lo.x+s->w || hi.y!=lo.y+s->h ||
    (unsigned)(uv.x*t->Width+.5f)!=s->x || (unsigned)(uv.y*t->Height+.5f)!=s->y ||
    (unsigned)(uv1.x*t->Width+.5f)!=s->x+s->w || (unsigned)(uv1.y*t->Height+.5f)!=s->y+s->h)failed++;
 ++draws;
}
static void draw(const WheelSprite*s,unsigned edge){
 expected_sprite=s;expected_edge=edge;
 draw_packed_sprite(0,s,11,19,edge,edge,0xabcdef01);
}
static void all(void){
 for(unsigned i=0;i<54;i++)for(unsigned lv=0;lv<4;lv++)draw(&control_sprites[i][lv],16u<<lv);
 for(unsigned i=0;i<100;i++)draw(&item_sprites[i],96);
    draw(&prowler_frame_normal,256);
    draw(&prowler_frame_selected,380);
    for(unsigned i=0;i<100;i++)if(prowler_icons[i].w)draw(&prowler_icons[i],96);
}
static int finish_frame(void){
 for(unsigned i=0;i<sizeof(wheel_pages)/sizeof(*wheel_pages);i++){
  ImTextureData*t=&((AtlasContext*)hooks[context_id].UserData)->textures[i];
  if(t->RefCount && t->Status==ImTextureStatus_WantCreate){
   if((unsigned)t->Width!=wheel_pages[i].width || (unsigned)t->Height!=wheel_pages[i].height)return 1;
   t->TexID=100+i;t->Status=ImTextureStatus_OK;
  }
 }
 return 0;
}
unsigned run_test(void){
 const unsigned n=sizeof(wheel_pages)/sizeof(*wheel_pages);
 all();if(failed||registrations!=n||!draws||finish_frame())return 1;
 unsigned initial_draws=draws;
 all();if(failed||draws!=2*initial_draws||registrations!=n)return 2;
 context_id=1;all();if(failed||registrations!=n*2||finish_frame())return 3;
 context_id=0;all();if(failed||registrations!=n*2)return 4;
 hooks[0].Callback(igGetCurrentContext(),&hooks[0]);if(failed||freed!=1||unregistrations!=n)return 5;
 all();if(failed||registrations!=n*3||finish_frame())return 6;
 context_id=1;all();if(failed||registrations!=n*3)return 7;
 hooks[1].Callback(igGetCurrentContext(),&hooks[1]);context_id=0;
 hooks[0].Callback(igGetCurrentContext(),&hooks[0]);
 return failed||atlas_contexts!=0||unregistrations!=3*n||freed!=3?8:0;
}
