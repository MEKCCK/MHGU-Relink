/* The port returns guest handles for host textures. Create, fill, register and
   obtain the draw reference through Forge, leaving the host descriptor opaque. */
static ImTextureData *texture_for_page(unsigned page){
    if(!atlas_ctx || page>=sizeof(wheel_pages)/sizeof(*wheel_pages))return 0;
    if(page_textures[page])return page_textures[page];
    if(pc_time<page_retry_at[page])return 0;
    const AtlasPage *data=&wheel_pages[page];
    ImTextureData *tex=ImTextureData_ImTextureData();
    if(tex){
        ImTextureData_Create(tex,ImTextureFormat_RGBA32,data->width,data->height);
        unsigned *out=ImTextureData_GetPixels(tex);
        if(out){
            unsigned count=data->width*data->height;
            for(unsigned i=0;i<count;i++)out[i]=data->pixels[i];
            igRegisterUserTexture(tex);
            page_textures[page]=tex;
            return tex;
        }
        ImTextureData_destroy(tex);
    }
    /* The guest pixel staging table is shared by every enabled plugin. */
    page_retry_at[page]=pc_time+10000000;
    return 0;
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
