// The runner includes the actual config parser and overlay_start from forge.c.
// Only Android storage, file lookup and overlay output are host fixtures.
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static std::string internal, font;
static bool storage_available = true;
static float font_size;
static int menu_key, log_level = 1;
static const char *SDL_GetAndroidInternalStoragePath() {
    return storage_available ? internal.c_str() : nullptr;
}
static bool ci_path(const char *root, const char *relative, char *out, size_t size) {
    std::string path = (fs::path(root) / relative).generic_string();
    if (!fs::is_regular_file(path)) return false;
    snprintf(out, size, "%s", path.c_str());
    return true;
}
static void overlay_init(const char *path, float size, int key) {
    font = path ? path : "";
    font_size = size;
    menu_key = key;
}
#define __ANDROID__ 1
#define strcasecmp _stricmp
#define LOG(...) ((void)0)
#include "forge_builtin.inc"

static void put(const fs::path &path, const char *text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    assert(out.good());
}
static void expect(const fs::path &path, float size, int key, int level) {
    assert(font == path.generic_string());
    assert(font_size == size && menu_key == key && log_level == level);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    internal = fs::path(argv[1]).generic_string();
    fs::path resources = fs::path(argv[2]) / "forge-v2.0.0/romfs/nativeNX/forge";
    fs::path builtin = fs::path(internal) / "forge-v2.0.0/romfs/nativeNX/forge";
    fs::path legacy = fs::path(internal) / "forge-v1.5.1/romfs/nativeNX/forge";
    const char *layers[] = {"game/romfs", "mod-a/romfs", "mod-b/romfs"};
    const char *config = "nativeNX/forge/config.ini", *ttf = "nativeNX/forge/fonts/Roboto-Medium.ttf";

    storage_available = false;
    overlay_start(nullptr, 0);
    expect({}, 12.f, 73, 1);
    storage_available = true;
    put(legacy / "config.ini", "[log]\nlevel=debug\n[menu]\nkey=55\nfont_size=17\n");
    put(legacy / "fonts/Roboto-Medium.ttf", "legacy private font");
    overlay_start(nullptr, 0);
    expect({}, 12.f, 73, 1);  // Old private defaults cannot shadow the new version.
    fs::create_directories(builtin);
    fs::copy(resources, builtin, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    overlay_start(layers, 3);
    expect(builtin / "fonts/Roboto-Medium.ttf", 12.f, 73, 3);

    put(fs::path(layers[0]) / config, "[log]\nlevel=warn\n[menu]\nkey=50\nfont_size=20\n");
    put(fs::path(layers[0]) / ttf, "game font");
    overlay_start(layers, 3);
    expect(fs::path(layers[0]) / ttf, 20.f, 50, 2);
    put(fs::path(layers[1]) / config, "[log]\nlevel=debug\n[menu]\nkey=51\nfont_size=18\n");
    overlay_start(layers, 3);
    expect(fs::path(layers[0]) / ttf, 18.f, 51, 0);
    put(fs::path(layers[2]) / ttf, "mod font");
    overlay_start(layers, 3);
    expect(fs::path(layers[2]) / ttf, 18.f, 51, 0);

    put(fs::path(layers[2]) / config, "[menu]\nkey=52\n");
    overlay_start(layers, 3);
    expect(fs::path(layers[2]) / ttf, 12.f, 52, 1);  // Files override; missing keys are not merged.

    // Preserve the existing whole-file override policy, including malformed files.
    put(fs::path(layers[2]) / config, "bad config\n");
    overlay_start(layers, 3);
    expect(fs::path(layers[2]) / ttf, 12.f, 73, 1);
    env_level = true;
    log_level = 2;
    overlay_start(nullptr, 0);
    expect(builtin / "fonts/Roboto-Medium.ttf", 12.f, 73, 2);
    env_level = false;
    internal += "/missing";
    overlay_start(nullptr, 0);
    expect({}, 12.f, 73, 1);
    puts("PASS: native Forge 2.0.0 defaults, legacy private defaults ignored, game/mod per-file precedence, whole-file overrides, malformed config and log override");
}
