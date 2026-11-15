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

static const uint32_t atlas=0x08100000u,other_copy=0x08200000u;
static const uint32_t font_atlas=0x08300000u,font_copy=0x08380000u;
typedef struct { float x,y,w,h; int slot; } Glyph;
// Measured HD_custom_pnl2 texel boxes; the native table's Up / Down / Right order is not slot order.
static const Glyph glyphs[]={{263,247,26,27,0},{327,246,26,27,1},{295,246,26,27,2}};
static const Glyph font_glyphs[]={{72,24,24,24,0},{96,24,24,24,1},{0,48,24,24,2}};

static void fixture(void) {
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

static void expect(uint32_t address,float u0,float v0,float u1,float v1,int slot) {
    const int actual=prompts_hunter_art_slot(address,u0,v0,u1,v1);
    if(actual!=slot) fprintf(stderr,"ART UV FAILURE: address=%08x uv=(%g,%g,%g,%g) expected=%d actual=%d\n",
        address,u0,v0,u1,v1,slot,actual);
    assert(actual==slot);
}

static void identity(void) {
    fixture();
    for(unsigned i=0;i<sizeof glyphs/sizeof *glyphs;++i) {
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
    sheets[1].copies[0]=sheets[1].copies[1]; sheets[1].ncopies=1;
    const Glyph g=glyphs[0];
    expect(atlas,g.x,g.y,g.x+g.w,g.y+g.h,-1);
    expect(other_copy,g.x,g.y,g.x+g.w,g.y+g.h,0);
    for(unsigned i=0;i<sizeof font_glyphs/sizeof *font_glyphs;++i) {
        const Glyph f=font_glyphs[i];
        for(int copy=0;copy<2;++copy) {
            const uint32_t address=copy?font_copy:font_atlas;
            expect(address,f.x,f.y,f.x+24,f.y+24,f.slot);
            expect(address,f.x+.5f,f.y+.5f,f.x+23.5f,f.y+23.5f,f.slot);
            expect(address,f.x+.5f,f.y,f.x+23.5f,f.y+24,f.slot);
            expect(address,f.x,f.y+.5f,f.x+24,f.y+23.5f,f.slot);
        }
    }
    puts("PASS: Hunter Art UV identity uses authenticated HD_custom_pnl2/button_festa copies and maps Up/Right/Down to slots 1/2/3, including font 23px half-texel and panel ink/size boundaries");
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
    for(int i=0;i<sheets[1].ncells;++i) {
        const Cell c=sheets[1].cells[i];
        if(c.btn==BTN_UP || c.btn==BTN_RIGHT || c.btn==BTN_DOWN) continue;
        expect(atlas,(float)c.x,(float)c.y,(float)(c.x+c.w),(float)(c.y+c.h),-1);
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
    puts("PASS: Hunter Art UV rejection excludes unknown/cross-sheet addresses, compound font arrows, nonfinite/normalized UVs, invalid sizes and crops outside panel/font tolerances");
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *names[]={"uv-identity","uv-rejection"};
    void (*run[])(void)={identity,rejection};
    int passed=0;
    for(int i=0;i<2;++i) {
        if(argc==2 && strcmp(argv[1],names[i])) continue;
        printf("{\"suite\":\"hunter-art-uv\",\"event\":\"case-start\",\"case\":\"%s\"}\n",names[i]);
        run[i](); ++passed;
        printf("{\"suite\":\"hunter-art-uv\",\"event\":\"case-result\",\"case\":\"%s\",\"status\":\"pass\"}\n",names[i]);
    }
    if(!passed || argc>2) { fprintf(stderr,"Use uv-identity or uv-rejection\n"); return 2; }
    printf("{\"suite\":\"hunter-art-uv\",\"event\":\"summary\",\"passed\":%d,\"status\":\"pass\",\"host_only\":true}\n",passed);
    return 0;
}
