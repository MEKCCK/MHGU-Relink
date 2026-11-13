// Generate real translator output for optional-extension and core sampling cases.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

static void generate(const char *dir, const char *name, int stage, int type, bool shadow, int lod_kind, bool required) {
    Ctx *c = calloc(1, sizeof *c);
    ShaderInfo *info = calloc(1, sizeof *info);
    ShaderKey key = {.glsl = true};
    c->glsl = true; c->stage = stage; c->info = info; c->key = &key; info->stage = stage;
    if (type >= 0) {
        info->ntex = 1; info->tex[0].type = type; info->tex[0].shadow = shadow;
        const char *co[] = {"0.25f", "0.5f", "1.0f"};
        const char *layer = type == TEXTYPE_1D_ARRAY || type == TEXTYPE_2D_ARRAY || type == TEXTYPE_CUBE_ARRAY ? "0u" : NULL;
        char sample[8192];
        glsl_sample(c, sample, 0, co, layer, shadow ? "0.5f" : NULL, lod_kind, "0.0f", NULL);
        assert(!c->fail);
        sp(&c->body, "    float4 sampled = %s;\n", sample);
    }
    assert(c->uses_shadow_lod == required);
    info = glsl_finish(c, false, false);
    assert(info->msl && !info->error[0]);
    assert(!strncmp(info->msl, "#version 460\n", 13));
    assert((strstr(info->msl, "GL_EXT_texture_shadow_lod : require") != NULL) == required);
    char path[2048]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *out = fopen(path, "wb"); assert(out);
    fputs(info->msl, out); fclose(out); maxwell_free(info);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *dir = argv[1];
    generate(dir, "empty.vert", STAGE_VERTEX, -1, false, 0, false);
    generate(dir, "empty.frag", STAGE_FRAGMENT, -1, false, 0, false);
    generate(dir, "empty.comp", STAGE_COMPUTE, -1, false, 0, false);
    generate(dir, "texture.vert", STAGE_VERTEX, TEXTYPE_2D, false, 0, false);
    generate(dir, "shadow2d.vert", STAGE_VERTEX, TEXTYPE_2D, true, 1, false);
    generate(dir, "shadow1darray.frag", STAGE_FRAGMENT, TEXTYPE_1D_ARRAY, true, 1, false);
    generate(dir, "shadowcube.frag", STAGE_FRAGMENT, TEXTYPE_CUBE, true, 0, false);
    generate(dir, "shadowarray.frag", STAGE_FRAGMENT, TEXTYPE_2D_ARRAY, true, 0, false);
    generate(dir, "shadowcubearray.frag", STAGE_FRAGMENT, TEXTYPE_CUBE_ARRAY, true, 0, false);
    generate(dir, "required-shadowcube.vert", STAGE_VERTEX, TEXTYPE_CUBE, true, 0, true);
    generate(dir, "required-shadowarray.frag", STAGE_FRAGMENT, TEXTYPE_2D_ARRAY, true, 1, true);
    generate(dir, "required-shadowcubearray.frag", STAGE_FRAGMENT, TEXTYPE_CUBE_ARRAY, true, 1, true);
    puts("PASS: optional extension omitted for core shaders and retained for explicit shadow LOD.");
}
