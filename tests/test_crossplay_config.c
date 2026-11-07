// Host regression: use a temporary MHGU_CONFIG path, never a live game directory.
#include "config.h"
#include "settings_locale.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define set_env(k, v) _putenv_s(k, v)
#else
#define set_env(k, v) setenv(k, v, 1)
#endif
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *path = argv[1];
    set_env("MHGU_CONFIG", path);
    FILE *f = fopen(path, "wb"); assert(f);
    fputs("; keep this comment\n[Future]\nKeep = preserved\n", f); fclose(f);
    Config initial = config_defaults();
    assert(initial.crossplay == XP_OFF && !initial.ryu_host && !initial.room_host);
    assert(initial.ryu_port == 30456 && initial.room_port == 24872);
    assert(!strcmp(initial.room_name, "MHGU"));
    const int touch_defaults[] = TOUCH_SHOW_DEFAULT;
    assert(!memcmp(initial.touch_show, touch_defaults, sizeof touch_defaults));
    assert(initial.touch_controls == TOUCH_AUTO && initial.touch_camera == 1.0f);
    assert(TOUCH_LEGACY_LAYOUTS == 4 && TOUCH_LAYOUT_PROFILES == 19);
    g_config = initial; config_load();
    Config c = config_editable();
    c.crossplay = XP_MIXED; c.ryu_host = 1; c.ryu_port = 30457;
    strcpy(c.ryu_servers, "192.0.2.10:30457,192.0.2.11");
    strcpy(c.ryu_password, "Ryujinx-1234abcd");
    c.room_host = 1; c.room_port = 24873;
    strcpy(c.room_server, "192.0.2.20:24873");
    strcpy(c.room_name, "Test room"); strcpy(c.room_password, "room-pass");
    char error[256], help[2048];
    assert(config_save(&c, error, sizeof error) && !config_restart_required(&c));
    const char *keys[] = {"Crossplay", "RyuServers", "RyuHost", "RyuPort", "RyuPassword",
                          "RoomServer", "RoomHost", "RoomPort", "RoomName", "RoomPassword"};
    Config live = {0}; live.volume = 73; config_live_crossplay(&live);
    assert(live.volume == 73);
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; ++i) {
        const ConfigSetting *setting = config_setting(keys[i]);
        assert(setting && !setting->restart && config_equal(setting, &c, &live));
        assert(config_help(keys[i], help, sizeof help) && *help);
        assert(settings_crossplay_help_zh(keys[i]));
    }
    g_config = initial; config_load(); Config loaded = config_editable();
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; ++i)
        assert(config_equal(config_setting(keys[i]), &c, &loaded));
    Config bad = c; bad.crossplay = 4; assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.ryu_port = 0; assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.room_port = 65536; assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.room_host = 2; assert(!config_save(&bad, error, sizeof error));
    bad = c; strcpy(bad.room_server, "host#comment"); assert(!config_save(&bad, error, sizeof error));
    bad = c; strcpy(bad.ryu_password, "pass;comment"); assert(!config_save(&bad, error, sizeof error));
    bad = c; memset(bad.room_name, 'a', sizeof bad.room_name); assert(!config_save(&bad, error, sizeof error));
    set_env("MHGU_CROSSPLAY", "1"); set_env("MHGU_RYU_PORT", "30500");
    g_config = initial; config_load(); config_live_crossplay(&live);
    assert(config_editable().crossplay == XP_MIXED && live.crossplay == XP_RYU && live.ryu_port == 30500);
    Config unrelated = config_editable(); unrelated.volume = 44;
    assert(config_save(&unrelated, error, sizeof error)); config_live_crossplay(&live);
    assert(live.crossplay == XP_RYU && live.ryu_port == 30500);
    set_env("MHGU_CROSSPLAY", "9"); set_env("MHGU_RYU_PORT", "70000");
    set_env("MHGU_ROOM_SERVER", "bad\nname");
    g_config = initial; config_load(); config_live_crossplay(&live);
    assert(live.crossplay == XP_MIXED && live.ryu_port == 30457);
    assert(!strcmp(live.room_server, c.room_server));
    assert(strcmp(settings_text(2, "Crossplay"), "Crossplay"));
    set_env("MHGU_CROSSPLAY", ""); set_env("MHGU_RYU_PORT", ""); set_env("MHGU_ROOM_SERVER", "");
    remove(path);
    puts("PASS: original touch defaults, crossplay config persistence, validation, live snapshots, temporary overrides and Chinese help");
    return 0;
}
