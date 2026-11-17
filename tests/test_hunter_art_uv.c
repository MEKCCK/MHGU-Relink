// Actual production atlas lookup; copies below represent already authenticated texture uploads.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
// The host header only defines mutexes; this pure lookup must never load any game archive.
typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT 0
static int pthread_once(pthread_once_t *once,void (*load)(void)) {
    (void)once; (void)load;
    assert(!"UV lookup must not initialize game assets"); return -1;
}
#endif

#include "../runtime/hle/prompts.c"

// Independent native-input fixtures; the production getter's option reads have their own ranged-input regression.
static uint64_t art_inputs[3];
uint64_t weapon_art_buttons(int slot) {
    assert(slot>=0 && slot<3);
    return art_inputs[slot];
}
static void controls(int type,int shoulders,int directions) {
    static const uint64_t native[4][2][2][3]={
        {{{0x2000,0x4000,0x8000},{0x200000,0x400000,0x800000}},
         {{0x2000,0x4000,0x8000},{0x200000,0x400000,0x800000}}},
        {{{0x83,0x100,0x200},{0x83,0x100,0x200}},
         {{0x203,0x40,0x80},{0x203,0x40,0x80}}},
        {{{0x104,0x101,0x102},{0x104,0x101,0x102}},
         {{0x44,0x41,0x42},{0x44,0x41,0x42}}},
        {{{0x200000,0x400000,0x800000},{0x2000,0x4000,0x8000}},
         {{0x200000,0x400000,0x800000},{0x2000,0x4000,0x8000}}},
    };
    memcpy(art_inputs,native[type][shoulders][directions],sizeof art_inputs);
}

static const uint32_t atlas=0x08100000u,other_copy=0x08200000u;
static const uint32_t font_atlas=0x08300000u,font_copy=0x08380000u;
typedef struct { float x,y,w,h; int slot; } Glyph;
// Measured atlas boxes; Up/Right/Down and type 3's X/A/B select the same three slots.
static const Glyph glyphs[]={{263,247,26,27,0},{327,246,26,27,1},{295,246,26,27,2},
                             {70,246,28,29,0},{135,246,28,29,1},{102,246,28,28,2}};
static const Glyph font_glyphs[]={{72,24,24,24,0},{96,24,24,24,1},{0,48,24,24,2},
                                  {24,0,24,24,0},{72,0,24,24,1},{48,0,24,24,2}};
typedef struct { int button; Glyph panel,font; } ButtonGlyph;
static const ButtonGlyph button_glyphs[]={
    {BTN_UP,{263,247,26,27,0},{72,24,24,24,0}},
    {BTN_RIGHT,{327,246,26,27,0},{96,24,24,24,0}},
    {BTN_DOWN,{295,246,26,27,0},{0,48,24,24,0}},
    {BTN_X,{70,246,28,29,0},{24,0,24,24,0}},
    {BTN_A,{135,246,28,29,0},{72,0,24,24,0}},
    {BTN_B,{102,246,28,28,0},{48,0,24,24,0}},
    {BTN_L,{5,247,29,26,0},{96,0,24,24,0}},
    {BTN_R,{167,247,28,26,0},{0,24,24,24,0}},
    {BTN_ZL,{198,247,28,27,0},{120,24,24,24,0}},
    {BTN_ZR,{229,247,29,27,0},{144,24,24,24,0}},
};

static void fixture(void) {
    controls(0,0,0);
    for(int i=0;i<NSHEETS;++i) {
        memset(sheets[i].copies,0,sizeof sheets[i].copies);
        sheets[i].ncopies=0;
    }
    sheets[1].copies[0]=(Copy){atlas,0};
    sheets[1].copies[1]=(Copy){other_copy,5};
    sheets[1].ncopies=2;
    sheets[0].copies[0]=(Copy){font_atlas,0};
    sheets[0].copies[1]=(Copy){font_copy,2};
    sheets[0].ncopies=2;
}

static int expect(uint32_t address,float u0,float v0,float u1,float v1,int slot) {
    const int actual=prompts_hunter_art_slot(address,u0,v0,u1,v1);
    if(actual!=slot) fprintf(stderr,"ART UV FAILURE: address=%08x uv=(%g,%g,%g,%g) expected=%d actual=%d\n",
        address,u0,v0,u1,v1,slot,actual);
    assert(actual==slot);
    return actual;
}
static int expect_button(int sheet,int button,int slot) {
    for(unsigned i=0;i<sizeof button_glyphs/sizeof *button_glyphs;++i) {
        const ButtonGlyph *b=&button_glyphs[i];
        if(b->button!=button) continue;
        const Glyph g=sheet?b->panel:b->font;
        return expect(sheet?atlas:font_atlas,g.x,g.y,g.x+g.w,g.y+g.h,slot);
    }
    assert(!"missing button UV fixture"); return -1;
}

static void identity(void) {
    fixture();
    for(unsigned i=0;i<sizeof glyphs/sizeof *glyphs;++i) {
        controls(i<3?0:2,0,0);
        const Glyph g=glyphs[i];
        for(int copy=0;copy<2;++copy) {
            const uint32_t address=copy?other_copy:atlas;
            expect(address,g.x,g.y,g.x+g.w,g.y+g.h,g.slot);
            expect(address,g.x+.5f,g.y+.5f,g.x+g.w-.5f,g.y+g.h-.5f,g.slot);
            expect(address,g.x-.5f,g.y-.5f,g.x+g.w+.5f,g.y+g.h+.5f,g.slot);
            // One texel is the inclusive ink-box tolerance; symmetric crop leaves width exactly 24.
            expect(address,g.x+1,g.y+1,g.x+g.w-1,g.y+g.h-1,g.slot);
            const float x=g.x+g.w*.5f,y=g.y+g.h*.5f;
            expect(address,x-20,y-20,x+20,y+20,g.slot);
        }
    }
    // A copy at index zero is not special, and forgetting its identity immediately disables it.
    controls(0,0,0);
    sheets[1].copies[0]=sheets[1].copies[1]; sheets[1].ncopies=1;
    const Glyph g=glyphs[0];
    expect(atlas,g.x,g.y,g.x+g.w,g.y+g.h,-1);
    expect(other_copy,g.x,g.y,g.x+g.w,g.y+g.h,0);
    for(unsigned i=0;i<sizeof font_glyphs/sizeof *font_glyphs;++i) {
        controls(i<3?0:2,0,0);
        const Glyph f=font_glyphs[i];
        for(int copy=0;copy<2;++copy) {
            const uint32_t address=copy?font_copy:font_atlas;
            expect(address,f.x,f.y,f.x+24,f.y+24,f.slot);
            expect(address,f.x+.5f,f.y+.5f,f.x+23.5f,f.y+23.5f,f.slot);
            expect(address,f.x+.5f,f.y,f.x+23.5f,f.y+24,f.slot);
            expect(address,f.x,f.y+.5f,f.x+24,f.y+23.5f,f.slot);
        }
    }
    puts("PASS: Hunter Art UV identity uses authenticated HD_custom_pnl2/button_festa copies and current native inputs, including font 23px half-texel and panel ink/size boundaries");
}

static void control_types(void) {
    // Button order is Up, Right, Down, X, A, B, L, R, ZL, ZR; these expectations do not derive from the masks.
    static const int slots[4][2][10]={
        {{0,1,2,-1,-1,-1,-1,-1,-1,-1},{0,1,2,-1,-1,-1,-1,-1,-1,-1}},
        {{-1,-1,-1,-1,0,0,-1,0,1,2},{-1,-1,-1,-1,0,0,1,2,-1,0}},
        {{-1,-1,-1,0,1,2,-1,-1,-1,-1},{-1,-1,-1,0,1,2,-1,-1,-1,-1}},
        {{0,1,2,-1,-1,-1,-1,-1,-1,-1},{0,1,2,-1,-1,-1,-1,-1,-1,-1}},
    };
    fixture();
    for(int type=0;type<4;++type) for(int shoulders=0;shoulders<2;++shoulders)
        for(int directions=0;directions<2;++directions) {
            controls(type,shoulders,directions);
            for(int sheet=0;sheet<2;++sheet) {
                for(unsigned i=0;i<sizeof button_glyphs/sizeof *button_glyphs;++i)
                    expect_button(sheet,button_glyphs[i].button,slots[type][shoulders][i]);
                if(type==1) {
                    const int chord[]={shoulders?BTN_ZR:BTN_R,BTN_A,BTN_B};
                    for(int reverse=0;reverse<2;++reverse) {
                        unsigned seen=0;
                        for(int i=0;i<3;++i) {
                            const int slot=expect_button(sheet,chord[reverse?2-i:i],0);
                            seen|=1u<<slot;
                        }
                        assert(seen==1); // The final B, or the reversed chord, cannot mark another Art slot.
                    }
                }
            }
        }
    memset(art_inputs,0,sizeof art_inputs); // Unknown settings fail closed, including formerly valid direction/face glyphs.
    for(int sheet=0;sheet<2;++sheet) for(int i=0;i<sheets[sheet].ncells;++i) {
        const Cell c=sheets[sheet].cells[i];
        expect(sheet?atlas:font_atlas,(float)c.x,(float)c.y,(float)(c.x+c.w),(float)(c.y+c.h),-1);
    }
    puts("PASS: both Art atlases cover all control types and shoulder/direction swaps; Type 2 chord order stays slot 1, unique shoulders identify slots 2/3, shared Type 3 modifiers and unknown settings are rejected");
}

static void rejection(void) {
    fixture();
    const Glyph g=glyphs[0];
    expect(0,g.x,g.y,g.x+g.w,g.y+g.h,-1);
    expect(atlas+1,g.x,g.y,g.x+g.w,g.y+g.h,-1);
    for(int i=0;i<NSHEETS;++i) {
        if(i<2) continue;
        const uint32_t address=0x08400000u+(uint32_t)i*0x100000u;
        sheets[i].copies[0]=(Copy){address,0}; sheets[i].ncopies=1;
        expect(address,g.x,g.y,g.x+g.w,g.y+g.h,-1);
    }
    for(int sheet=0;sheet<2;++sheet) for(int i=0;i<sheets[sheet].ncells;++i) {
        const Cell c=sheets[sheet].cells[i];
        if(c.btn2<0 && (c.btn==BTN_UP || c.btn==BTN_RIGHT || c.btn==BTN_DOWN)) continue;
        expect(sheet?atlas:font_atlas,(float)c.x,(float)c.y,(float)(c.x+c.w),(float)(c.y+c.h),-1);
    }
    const float invalid[]={NAN,INFINITY,-INFINITY};
    for(unsigned bad=0;bad<sizeof invalid/sizeof *invalid;++bad) for(int component=0;component<4;++component) {
        float uv[]={g.x,g.y,g.x+g.w,g.y+g.h}; uv[component]=invalid[bad];
        expect(atlas,uv[0],uv[1],uv[2],uv[3],-1);
    }
    expect(atlas,INFINITY,INFINITY,INFINITY,INFINITY,-1);
    expect(atlas,-INFINITY,-INFINITY,INFINITY,INFINITY,-1);
    expect(atlas,g.x/512,g.y/512,(g.x+g.w)/512,(g.y+g.h)/512,-1);
    expect(atlas,g.x,g.y,g.x,g.y+g.h,-1);
    expect(atlas,g.x,g.y,g.x+g.w,g.y,-1);
    expect(atlas,g.x+g.w,g.y,g.x,g.y+g.h,-1);
    expect(atlas,g.x,g.y+g.h,g.x+g.w,g.y,-1);
    for(unsigned i=0;i<sizeof glyphs/sizeof *glyphs;++i) {
        controls(i<3?0:2,0,0);
        const Glyph cell=glyphs[i];
        const float x=cell.x+cell.w*.5f,y=cell.y+cell.h*.5f;
        expect(atlas,x-11.99f,y-16,x+11.99f,y+16,-1);
        expect(atlas,x-16,y-11.99f,x+16,y+11.99f,-1);
        expect(atlas,x-20.01f,y-16,x+20.01f,y+16,-1);
        expect(atlas,x-16,y-20.01f,x+16,y+20.01f,-1);
        expect(atlas,cell.x+1.01f,cell.y,cell.x+cell.w,cell.y+cell.h,-1);
        expect(atlas,cell.x,cell.y+1.01f,cell.x+cell.w,cell.y+cell.h,-1);
        expect(atlas,cell.x,cell.y,cell.x+cell.w-1.01f,cell.y+cell.h,-1);
        expect(atlas,cell.x,cell.y,cell.x+cell.w,cell.y+cell.h-1.01f,-1);
    }
    expect(atlas,0,0,512,512,-1);
    expect(atlas,256,100,288,132,-1);
    expect(font_atlas,g.x,g.y,g.x+g.w,g.y+g.h,-1);
    for(unsigned i=0;i<sizeof font_glyphs/sizeof *font_glyphs;++i) {
        controls(i<3?0:2,0,0);
        const Glyph f=font_glyphs[i];
        expect(atlas,f.x,f.y,f.x+24,f.y+24,-1);
        expect(font_atlas+1,f.x,f.y,f.x+24,f.y+24,-1);
        expect(font_atlas,f.x+.51f,f.y+.5f,f.x+23.49f,f.y+23.5f,-1);
        expect(font_atlas,f.x+.5f,f.y+.51f,f.x+23.5f,f.y+23.49f,-1);
        expect(font_atlas,f.x-.01f,f.y,f.x+24,f.y+24,-1);
        expect(font_atlas,f.x,f.y-.01f,f.x+24,f.y+24,-1);
        expect(font_atlas,f.x+1,f.y+1,f.x+23,f.y+23,-1);
    }
    // The font's KEY_UD and KEY_RL compound arrows must never impersonate one Art slot.
    const float compound_x[]={48,72};
    for(unsigned i=0;i<sizeof compound_x/sizeof *compound_x;++i) {
        const float x=compound_x[i];
        expect(font_atlas,x,48,x+24,72,-1);
        expect(font_atlas,x+.5f,48.5f,x+23.5f,71.5f,-1);
    }
    puts("PASS: Hunter Art UV rejection excludes other panel/font symbols, unknown/cross-sheet addresses, compound font arrows, nonfinite/normalized UVs, invalid sizes and crops outside panel/font tolerances");
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *names[]={"uv-identity","uv-rejection","uv-control-types"};
    void (*run[])(void)={identity,rejection,control_types};
    int passed=0;
    for(int i=0;i<3;++i) {
        if(argc==2 && strcmp(argv[1],names[i])) continue;
        printf("{\"suite\":\"hunter-art-uv\",\"event\":\"case-start\",\"case\":\"%s\"}\n",names[i]);
        run[i](); ++passed;
        printf("{\"suite\":\"hunter-art-uv\",\"event\":\"case-result\",\"case\":\"%s\",\"status\":\"pass\"}\n",names[i]);
    }
    if(!passed || argc>2) { fprintf(stderr,"Use uv-identity, uv-rejection or uv-control-types\n"); return 2; }
    printf("{\"suite\":\"hunter-art-uv\",\"event\":\"summary\",\"passed\":%d,\"status\":\"pass\",\"host_only\":true}\n",passed);
    return 0;
}
