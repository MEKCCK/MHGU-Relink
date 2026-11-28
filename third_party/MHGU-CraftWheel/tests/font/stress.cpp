// Headless CPU atlas regression for the same Dear ImGui version as wheelcore.
#include "imgui.h"
#include "imgui_internal.h"
#include "item_names.inc"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <initializer_list>

static size_t current_bytes, peak_bytes, largest_block, budget;
static unsigned frames;
static int peak_texture_width, peak_texture_height, peak_sizes;
static int peak_tex_bytes;
static float first_size=24.0f, second_size=36.0f;
static void* tracked_allocate(size_t bytes, void*) {
    if (budget && bytes + current_bytes > budget) {
        std::printf("budget exceeded: frame=%u live=%zu request=%zu cap=%zu\n", frames, current_bytes, bytes, budget);
        std::fflush(stdout);
        std::_Exit(9);
    }
    auto allocation = (size_t*)std::malloc(bytes + 16);
    assert(allocation);
    allocation[0] = bytes;
    current_bytes += bytes;
    if (peak_bytes < current_bytes) peak_bytes = current_bytes;
    if (largest_block < bytes) largest_block = bytes;
    return (unsigned char*)allocation + 16;
}
static void tracked_free(void* ptr, void*) {
    if (!ptr) return;
    auto head = (size_t*)((unsigned char*)ptr - 16);
    current_bytes -= head[0];
    std::free(head);
}
static void end_frame(ImFont* font, bool lock) {
    ImGui::Render();
    ImDrawData* data = ImGui::GetDrawData();
    assert(data && data->TotalVtxCount > 0);
    for (ImTextureData* tex : *data->Textures) {
        if (tex->Pixels && tex->GetSizeInBytes() > peak_tex_bytes) {
            peak_tex_bytes = tex->GetSizeInBytes();
            peak_texture_width = tex->Width;
            peak_texture_height = tex->Height;
        }
        if (tex->Status == ImTextureStatus_WantCreate) {
            tex->SetTexID((ImTextureID)tex->UniqueID+1);
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    int sizes = 0;
    auto& pool = font->OwnerAtlas->Builder->BakedPool;
    for (int i=0; i<pool.Size; ++i) {
        auto& baked=pool[i];
        if (baked.OwnerFont == font && !baked.WantDestroy) {
            ++sizes;
            if (lock) assert(baked.Size == first_size || baked.Size == second_size);
        }
    }
    if (lock) assert(sizes == (first_size==second_size ? 1 : 2));
    if (peak_sizes < sizes) peak_sizes = sizes;
}
static const char* labels[] = {
    "快捷轮盘设置", "当前道具组", "预设列表", "选择", "清除", "返回",
    "调整轮盘位置", "自定义呼出轮盘按键", "推动摇杆触发", "设置按键图标风格",
    "左摇杆移动，或触屏拖动轮盘", "保存  撤销  恢复默认", "配置快捷轮盘"
};
int main(int argc,char** argv) {
    if (argc<3) return 1;
    const bool lock = std::strcmp(argv[1],"fixed")==0;
    budget = argc>3? std::strtoul(argv[3],nullptr,10)*1024u*1024u : 0;
    if (argc>5) {first_size=(float)std::atoi(argv[4]); second_size=(float)std::atoi(argv[5]);}
    ImGui::SetAllocatorFunctions(tracked_allocate,tracked_free);
    ImGui::CreateContext();
    auto& io=ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    if (argc>6) cfg.OversampleH=cfg.OversampleV=std::atoi(argv[6]);
    auto font=io.Fonts->AddFontFromFileTTF(argv[2],22.0f,&cfg);
    assert(font);
    unsigned item_count=IM_ARRAYSIZE(item_names);
    for (const float scale : {1.0f,1.5f,3.0f,1.0f}) {
        io.DisplaySize = ImVec2(1280*scale,720*scale);
        for (unsigned page=0; page<(item_count+15)/16; ++page) {
            ++frames;
            ImGui::NewFrame();
            if (frames==1 && lock) {
                assert(font->GetFontBaked(first_size,-1.0f));
                assert(font->GetFontBaked(second_size,-1.0f));
                font->Flags |= ImFontFlags_LockBakedSizes;
            }
            auto draw = ImGui::GetForegroundDrawList();
            for (unsigned i=0; i<IM_ARRAYSIZE(labels); ++i)
                draw->AddText(font,(14+(i%7)*2)*scale,ImVec2(20*scale,(20+i*25)*scale),0xffffffffu,labels[i]);
            for (unsigned i=0; i<16 && page*16+i<item_count; ++i)
                draw->AddText(font,18.0f*scale,ImVec2(300*scale,(10+i*24)*scale),0xffffffffu,item_names[page*16+i]);
            // Repeated push/release animation, as in the hunt overlay.
            float t=(page%11)/10.0f;
            float eased=t*t*(3.0f-2.0f*t);
            if (14*eased*scale >= 1.0f)
                draw->AddText(font,14*eased*scale,ImVec2(100*scale,460*scale),0xffffffffu,item_names[page%item_count]);
            if (18*eased*scale >= 1.0f)
                draw->AddText(font,18*eased*scale,ImVec2(110*scale,490*scale),0xffffffffu,"1234567890");
            auto baked=font->GetFontBaked(18.0f*scale);
            auto glyph = baked->FindGlyphNoFallback(0x8f6e); // 轮
            assert(glyph && glyph->Visible && glyph->U1>glyph->U0 && glyph->V1>glyph->V0);
            if (page==0) {
                auto tex=font->OwnerAtlas->TexData;
                assert(tex && tex->Pixels && tex->Format==ImTextureFormat_RGBA32);
                bool antialiased=false;
                for (int y=int(glyph->V0*tex->Height); y<int(glyph->V1*tex->Height); ++y)
                    for (int x=int(glyph->U0*tex->Width); x<int(glyph->U1*tex->Width); ++x) {
                        auto alpha=static_cast<unsigned char*>(tex->GetPixelsAt(x,y))[3];
                        if (alpha>0 && alpha<255) antialiased=true;
                    }
                assert(antialiased);
            }
            end_frame(font,lock);
        }
        std::printf("scale=%.1f frames=%u peak_bytes=%zu max_texture=%dx%d largest_request=%zu peak_sizes=%d\n",
                    (double)scale,frames,peak_bytes,peak_texture_width,peak_texture_height,largest_block,peak_sizes);
        std::fflush(stdout);
    }
    ImGui::DestroyContext();
    assert(current_bytes==0);
}
