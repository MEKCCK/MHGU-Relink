/* Host regression for the guest helper. Pass the distributed font as argv[1]. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { int Flags; } ImFont;
typedef struct { int OversampleH, OversampleV; } ImFontConfig;
typedef struct { int unused; } ImFontAtlas;
typedef struct { ImFontAtlas *Fonts; } ImGuiIO;
typedef FILE *ImFileHandle;
typedef uint64_t ImU64;
enum { ImFontFlags_LockBakedSizes = 8 };
static ImFont loaded_font, overlay_font, *font;
static ImFontConfig cfg;
static ImFontAtlas atlas;
static ImGuiIO io = { &atlas };
static int loads, deletes, bakes;
static const char *source_path;
static bool malformed;

static FILE *igImFileOpen(const char *path, const char *mode)
{
    assert(strcmp(path, "app:/nativeNX/plugins/gu_craft_wheel_font.ttc") == 0);
    assert(strcmp(mode, "rb") == 0);
    if (malformed) {
        FILE *fp = tmpfile();
        assert(fp);
        for (int i = 0; i < 16; i++) fputc(0xff, fp);
        rewind(fp);
        return fp;
    }
    return source_path ? fopen(source_path, mode) : NULL;
}

static ImU64 igImFileGetSize(FILE *fp)
{
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    return (ImU64)size;
}

static ImU64 igImFileRead(void *out, ImU64 size, ImU64 count, FILE *fp)
{ return fread(out, (size_t)size, (size_t)count, fp); }
static void igImFileClose(FILE *fp) { assert(fclose(fp) == 0); }
static ImGuiIO *igGetIO_Nil(void) { return &io; }
static float igGetFontSize(void) { return 17.0f; }
static ImFont *igGetFont(void) { return &overlay_font; }
static ImFontConfig *ImFontConfig_ImFontConfig(void) { return &cfg; }
static void ImFontConfig_destroy(ImFontConfig *p) { assert(p == &cfg); ++deletes; }
static ImFont *ImFontAtlas_AddFontFromFileTTF(ImFontAtlas *a, const char *path,
                                             float size, ImFontConfig *config, void *ranges)
{
    assert(source_path && !malformed && a == &atlas && size == 22 && !ranges);
    assert(strcmp(path, "app:/nativeNX/plugins/gu_craft_wheel_font.ttc") == 0);
    assert(config == &cfg && cfg.OversampleH == 1 && cfg.OversampleV == 1);
    ++loads;
    return &loaded_font;
}
static void *ImFont_GetFontBaked(ImFont *f, float size, float density)
{
    assert(f == &loaded_font && density == -1.0f && f->Flags == 0);
    assert(size == (bakes == 0 ? 24.0f : 36.0f));
    ++bakes;
    return f;
}
#define forge_log(...) ((void)0)
#include "port_font.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    float pixels = 21.75f;
    load_wheel_font(&io);
    assert(!font && !loads && wheel_text_font(&pixels) == &overlay_font && pixels == 17.0f);
    malformed = true;
    load_wheel_font(&io);
    assert(!font && !loads);
    malformed = false;
    source_path = argv[1];
    load_wheel_font(&io);
    load_wheel_font(&io);
    assert(font == &loaded_font && loads == 1 && deletes == 1);
    pixels = 21.75f;
    assert(wheel_text_font(&pixels) == font && pixels == 21.75f);
    pixels = 62.5f;
    assert(wheel_text_font(&pixels) == font && pixels == 62.5f);
    assert(bakes == 2 && loaded_font.Flags == 8 && overlay_font.Flags == 0);
    puts("PASS missing font, malformed header, packaged font, repeated init, fractional draw sizes");
}
