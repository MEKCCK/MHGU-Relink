/* Forge's generated HLE maps ImFontConfig and ImFont into typed guest mirrors.
   Only touch the mapped scalar fields; the native atlas vector is host-owned. */
static void *wheel_font_context;
static ImFontAtlas *wheel_font_atlas;
static bool wheel_font_sizes_ready;

static uint32_t font_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool wheel_font_readable(const char *path)
{
    /* The same Forge file bridge as AddFontFromFileTTF resolves app:/ mods. */
    ImFileHandle file = igImFileOpen(path, "rb");
    if (!file)
        return false;
    unsigned char header[16];
    ImU64 size = igImFileGetSize(file);
    ImU64 read = igImFileRead(header, 1, sizeof(header), file);
    igImFileClose(file);
    if (read != sizeof(header) || size > 0x7fffffff || size < sizeof(header))
        return false;
    uint32_t tag = font_be32(header);
    if (tag == 0x74746366) { /* ttcf, first face */
        uint32_t version = font_be32(header + 4);
        uint32_t faces = font_be32(header + 8);
        uint32_t first = font_be32(header + 12);
        return (version == 0x00010000 || version == 0x00020000) &&
               faces && faces <= (size - 12) / 4 &&
               first >= 12 + faces * 4 && first <= size - 12;
    }
    unsigned tables = ((unsigned)header[4] << 8) | header[5];
    return (tag == 0x00010000 || tag == 0x4f54544f || tag == 0x74727565) &&
           tables && 12 + 16 * tables <= size;
}

static void load_wheel_font(void *context)
{
    ImGuiIO *io = igGetIO_Nil();
    ImFontAtlas *atlas = io ? io->Fonts : 0;
    if (font && context == wheel_font_context && atlas == wheel_font_atlas)
        return;
    font = 0;
    wheel_font_sizes_ready = false;
    wheel_font_context = context;
    wheel_font_atlas = atlas;
    if (!atlas)
        return;

    const char *path = "app:/nativeNX/plugins/gu_craft_wheel_font.ttc";
    if (!wheel_font_readable(path)) {
        forge_log(3, "[CraftWheel] wheel font missing or unreadable, using overlay font");
        return;
    }
    ImFontConfig *config = ImFontConfig_ImFontConfig();
    if (!config)
        return;
    config->OversampleH = 1;
    config->OversampleV = 1;
    font = ImFontAtlas_AddFontFromFileTTF(atlas, path, 22.0f, config, 0);
    ImFontConfig_destroy(config);
    forge_log(3, "[CraftWheel] font=%08X", (uint32_t)font);
}

static ImFont *wheel_text_font(float *pixels)
{
    if (!font) {
        /* Borrow the overlay font at its current size if the TTC failed to load. */
        *pixels = igGetFontSize();
        return *pixels > 0.0f ? igGetFont() : 0;
    }
    if (!wheel_font_sizes_ready) {
        /* ImGui initializes its builder in NewFrame, after Forge's init callback. */
        if (!ImFont_GetFontBaked(font, 24.0f, -1.0f) ||
            !ImFont_GetFontBaked(font, 36.0f, -1.0f))
            return 0;
        font->Flags |= ImFontFlags_LockBakedSizes;
        wheel_font_sizes_ready = true;
    }
    return font;
}
