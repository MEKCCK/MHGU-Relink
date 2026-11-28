/* All pixels belong to immutable pages in this NRO. The private NVN backend
   reads them in WriteTexels. RegisterUserTexture borrows the description
   and never transfers ownership of either the description or its pixels. */
typedef struct AtlasContext {
    ImGuiContext *owner;
    struct AtlasContext *next;
    ImTextureData textures[sizeof(wheel_pages) / sizeof(wheel_pages[0])];
} AtlasContext;

static AtlasContext *atlas_contexts;

static void release_atlas_context(ImGuiContext *ctx, ImGuiContextHook *hook){
    AtlasContext *state=hook->UserData;
    for(unsigned i=0;i<sizeof(state->textures)/sizeof(state->textures[0]);i++){
        ImTextureData *tex=&state->textures[i];
        if(tex->RefCount){
            igUnregisterUserTexture(tex);
            tex->Pixels=0; /* Never pass NRO storage to an allocator. */
        }
    }
    AtlasContext **link=&atlas_contexts;
    while(*link && *link!=state)link=&(*link)->next;
    if(*link)*link=state->next;
    igMemFree(state);
    (void)ctx;
}

static ImTextureData *texture_for_page(unsigned page){
    ImGuiContext *ctx=igGetCurrentContext();
    if(!ctx)return 0;
    AtlasContext *state=atlas_contexts;
    while(state && state->owner!=ctx)state=state->next;
    if(!state){
        state=igMemAlloc(sizeof(*state));
        if(!state)return 0;
        memset(state,0,sizeof(*state));
        state->owner=ctx;
        state->next=atlas_contexts;
        atlas_contexts=state;
        ImGuiContextHook hook={.Type=ImGuiContextHookType_Shutdown,
                               .Callback=release_atlas_context,.UserData=state};
        igAddContextHook(ctx,&hook);
    }
    ImTextureData *tex=&state->textures[page];
    if(!tex->RefCount){
        const AtlasPage *p=&wheel_pages[page];
        tex->Width=p->width;
        tex->Height=p->height;
        tex->Format=ImTextureFormat_RGBA32;
        tex->BytesPerPixel=4;
        tex->TexID=ImTextureID_Invalid;
        tex->Pixels=(unsigned char*)p->pixels;
        tex->UpdateRect.x=tex->UpdateRect.y=0xffff;
        tex->Status=ImTextureStatus_WantCreate;
        igRegisterUserTexture(tex);
    }
    return tex;
}

static void draw_packed_sprite(ImDrawList*d,const WheelSprite*s,float x,float y,
                               float size,unsigned original_edge,ImU32 tint){
    if(!s->w)return;
    ImTextureData *tex=texture_for_page(s->page);
    if(!tex)return;
    const AtlasPage *p=&wheel_pages[s->page];
    float px=size/original_edge;
    x+=s->left*px;
    y+=s->top*px;
    ImVec2_c uv0={(float)s->x/p->width,(float)s->y/p->height};
    ImVec2_c uv1={(float)(s->x+s->w)/p->width,(float)(s->y+s->h)/p->height};
    ImDrawList_AddImage(d,ImTextureData_GetTexRef(tex),v(x,y),v(x+px*s->w,y+px*s->h),uv0,uv1,pc_color(tint));
}
