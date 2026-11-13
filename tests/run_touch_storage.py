"""Check Android touch saves against real production functions on a Windows host.

Only the layout data and POSIX replacement operation are fixtures. The helper and
both saving callers are extracted unchanged from runtime/touch.cpp. SDL's Android
relative-path prefix is modeled with a separate internal directory; no SDL DLL,
APK, game files, or external repository source is needed.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
OUTPUT = PROJECT / 'analysis/relink-touch-build/host-touch-storage'


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:bool|void)\s+' + re.escape(name)
                      + r'\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('Production function not found: ' + name)
    depth = 0
    for end in range(source.index('{', match.start()), len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[match.start():end + 1] + '\n'
    raise AssertionError('Unclosed production function: ' + name)


FIXTURE = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
static fs::path internal;
static bool fail_commit;
static int sdl_calls, posix_calls;

static std::string read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
static void write_file(const fs::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary);
    assert(file);
    file << text;
    file.close();
    assert(file);
}
// Windows CRT rename cannot replace an existing file. This fixture supplies
// POSIX replacement semantics without deleting the old target before commit.
static bool replace_file(const fs::path &from, const fs::path &to) {
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 1; attempt <= 10; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (attempt > 1)
                fprintf(stderr, "FIXTURE MoveFileExW recovered: attempts=%d previous_error=%lu\n",
                        attempt, static_cast<unsigned long>(error));
            return true;
        }
        error = GetLastError();
        fprintf(stderr, "FIXTURE MoveFileExW failed: error=%lu attempt=%d from=%s to=%s\n",
                static_cast<unsigned long>(error), attempt,
                from.filename().string().c_str(), to.filename().string().c_str());
        if (attempt == 10 || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)) break;
        Sleep(10);  // Only the Windows fixture waits out brief host access/sharing conflicts.
    }
    SetLastError(error);
    return false;
}
static int cwd_rename(const char *from, const char *to) {
    ++posix_calls;
    assert(!fs::path(from).is_absolute() && !fs::path(to).is_absolute());
    assert(fs::is_regular_file(from));  // fopen wrote the temporary file in cwd.
    if (fail_commit) { errno = EACCES; return -1; }
    return replace_file(from, to) ? 0 : -1;
}
// SDL 3.4.18's Android filesystem redirects relative rename paths to its app
// internal storage. libc fopen instead follows the game's chosen files cwd.
static bool SDL_RenamePath(const char *from, const char *to) {
    ++sdl_calls;
    assert(!fs::path(from).is_absolute() && !fs::path(to).is_absolute());
    return replace_file(internal / from, internal / to);
}

// Minimal data fixture: storage behavior does not depend on the number of HUD
// controls or their geometry. The production serializers themselves stay intact.
static constexpr int C_LAID = 1, TZ_COUNT = 1;
static constexpr int TOUCH_WEAPON_LAYOUTS = 15, UNIVERSAL_LAYOUT = 19;
static const struct { const char *key; } DEFS[] = {{"test_button"}};
static const char *ZONE_KEY[] = {"test_zone"};
struct Layout {
    float x[C_LAID]{.25f}, y[C_LAID]{.5f}, scale[C_LAID]{1};
    float zx[TZ_COUNT]{12}, zy[TZ_COUNT]{24};
    int style = 0, weapon_profile = 6;
};
struct State {
    int layout_profile = UNIVERSAL_LAYOUT;
    Layout layout;
    int weapon_layout[TOUCH_WEAPON_LAYOUTS];
    int selected_layout = -1;
};
static bool reserved_layout(int profile) { return profile >= 20 && profile < 24; }

// System headers must see the host platform. Only the extracted production
// functions below see Android/POSIX, even though this probe runs on Windows.
#undef _WIN32
#define __ANDROID__ 1
#define rename cwd_rename
'''


CHECKS = r'''
#undef rename

static void calls(bool reference) {
    assert(sdl_calls == (reference ? 1 : 0));
    assert(posix_calls == (reference ? 0 : 1));
    sdl_calls = posix_calls = 0;
}
static void untouched_internal(const char *path) {
    assert(read_file(internal / path) == "internal sentinel\n");
    assert(!fs::exists(internal / (std::string(path) + ".tmp")));
}
static void layout_saves(bool reference) {
    for (int profile : {0, 1, UNIVERSAL_LAYOUT, 20}) {
        char path[40]; layout_path(profile, path, sizeof path);
        const std::string temp = std::string(path) + ".tmp";
        write_file(path, "old layout\n");
        write_file(internal / path, "internal sentinel\n");
        Layout layout;
        assert(save_layout(layout, profile) == !reference);
        calls(reference);
        assert(!fs::exists(temp));
        untouched_internal(path);
        if (reference) {
            assert(read_file(path) == "old layout\n");
            continue;
        }
        assert(read_file(path).find("test_button 0.2500 0.5000 1.000\n") != std::string::npos);
        if (reserved_layout(profile))
            assert(read_file(path).find("weapon_profile 6 0 1\n") != std::string::npos);
        layout.x[0] = .75f;
        assert(save_layout(layout, profile));  // Replace an already saved file.
        calls(false);
        const std::string saved = read_file(path);
        assert(saved.find("test_button 0.7500 0.5000 1.000\n") != std::string::npos);
        fail_commit = true;
        layout.x[0] = .9f;
        assert(!save_layout(layout, profile));
        calls(false);
        assert(read_file(path) == saved && !fs::exists(temp));
        untouched_internal(path);
        fail_commit = false;
        assert(save_layout(layout, profile));  // A failed commit remains retryable.
        calls(false);
        assert(read_file(path).find("test_button 0.9000 0.5000 1.000\n") != std::string::npos);
        assert(!fs::exists(temp));
        untouched_internal(path);
    }
    puts(reference ? "PASS baseline: libc temporary files cannot be committed by Android-prefixed SDL rename"
                   : "PASS fixed: every layout filename replaces cwd data; failed commits preserve it and allow retry");
}
static std::string mapping_text(const int *mapping, int selected = -1) {
    std::string text;
    for (int weapon = 0; weapon < TOUCH_WEAPON_LAYOUTS; ++weapon)
        text += std::to_string(weapon) + " " + std::to_string(mapping[weapon]) + "\n";
    if (selected >= 0) text += "selected " + std::to_string(selected) + "\n";
    return text;
}
static void preset_saves(bool reference) {
    const char *path = "touch_weapon_presets.txt", *temp = "touch_weapon_presets.txt.tmp";
    State state;
    for (int weapon = 0; weapon < TOUCH_WEAPON_LAYOUTS; ++weapon) state.weapon_layout[weapon] = weapon;
    const std::string original = mapping_text(state.weapon_layout);
    write_file(path, original);
    write_file(internal / path, "internal sentinel\n");
    assert(activate_layout(state, state.layout_profile, state.layout.weapon_profile) == !reference);
    calls(reference);
    assert(!fs::exists(temp));
    untouched_internal(path);
    if (reference) {
        assert(read_file(path) == original && mapping_text(state.weapon_layout) == original && state.selected_layout == -1);
        puts("PASS baseline: preset activation fails without changing disk or selected weapons");
        return;
    }
    for (int selected : state.weapon_layout) assert(selected == UNIVERSAL_LAYOUT);
    assert(state.selected_layout == UNIVERSAL_LAYOUT);
    const std::string universal = mapping_text(state.weapon_layout, state.selected_layout);
    assert(read_file(path) == universal);
    state.layout_profile = 20;
    state.layout.weapon_profile = 6;
    fail_commit = true;
    assert(!activate_layout(state, state.layout_profile, state.layout.weapon_profile));
    calls(false);
    assert(read_file(path) == universal && mapping_text(state.weapon_layout, state.selected_layout) == universal);
    assert(state.selected_layout == UNIVERSAL_LAYOUT);
    assert(!fs::exists(temp));
    untouched_internal(path);
    fail_commit = false;
    assert(activate_layout(state, state.layout_profile, state.layout.weapon_profile));
    calls(false);
    for (int weapon = 0; weapon < TOUCH_WEAPON_LAYOUTS; ++weapon)
        assert(state.weapon_layout[weapon] == (weapon == 6 ? 20 : UNIVERSAL_LAYOUT));
    assert(state.selected_layout == 20);
    assert(read_file(path) == mapping_text(state.weapon_layout, state.selected_layout) && !fs::exists(temp));
    untouched_internal(path);
    assert(activate_layout(state, 0, 0));
    calls(false);
    assert(state.selected_layout == 0);
    const std::string fixed = mapping_text(state.weapon_layout, state.selected_layout);
    assert(read_file(path) == fixed);
    // Slot 20 is already this weapon's legacy mapping: only the global selection changes.
    fail_commit = true;
    assert(!activate_layout(state, 20, 6));
    calls(false);
    assert(state.selected_layout == 0 && read_file(path) == fixed);
    assert(mapping_text(state.weapon_layout, state.selected_layout) == fixed && !fs::exists(temp));
    untouched_internal(path);
    fail_commit = false;
    const std::string legacy = mapping_text(state.weapon_layout);
    assert(activate_layout(state, 20, 6));
    calls(false);
    assert(state.selected_layout == 20 && mapping_text(state.weapon_layout) == legacy);
    assert(read_file(path) == mapping_text(state.weapon_layout, state.selected_layout) && !fs::exists(temp));
    untouched_internal(path);
    puts("PASS fixed: preset activation atomically commits legacy mappings and global selection to cwd, preserves both on failure including selection-only changes, and allows retry");
}
int main() {
    assert(_set_fmode(_O_BINARY) == 0);  // Android fopen("w") does not translate LF.
    internal = fs::current_path().parent_path() / "internal";
    assert(fs::current_path().filename() == "player" && fs::is_directory(internal));
    const bool reference = REFERENCE_BUILD != 0;
    layout_saves(reference);
    preset_saves(reference);
    return 0;
}
'''


def main():
    source = (PROJECT / 'runtime/touch.cpp').read_text(encoding='utf-8')
    helper = function(source, 'commit_layout_file')
    callers = ''.join(function(source, name)
                      for name in ('layout_path', 'save_layout', 'activate_layout'))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    vcvars = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    setup = subprocess.run('call "' + str(vcvars) + '" >nul && set', shell=True,
                           capture_output=True, text=True, check=True)
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, separator, value = line.partition('=')
        if separator and key:
            env[key] = value
    compiler = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    probe = OUTPUT / 'touch-storage-probe.cpp'
    # The baseline deliberately restores only the old helper implementation;
    # both builds execute the current, unchanged production saving callers.
    baseline = 'bool commit_layout_file(const char *temp, const char *path) { return SDL_RenamePath(temp, path); }\n'
    probe.write_text(FIXTURE + '\n#if REFERENCE_BUILD\n' + baseline + '#else\n'
                     + helper + '#endif\n' + callers + CHECKS, encoding='utf-8')
    records = []
    for reference in (True, False):
        name = 'baseline' if reference else 'fixed'
        exe = OUTPUT / ('touch-storage-' + name + '.exe')
        build = subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link',
                                '-std=c++17', '-D_CRT_SECURE_NO_WARNINGS',
                                '-DREFERENCE_BUILD=' + str(int(reference)), str(probe), '-o', str(exe)],
                               env=env, cwd=OUTPUT, capture_output=True, timeout=120)
        (OUTPUT / (name + '-build.log')).write_bytes(build.stdout + build.stderr)
        if build.returncode:
            print((build.stdout + build.stderr).decode(errors='replace'), end='')
        build.check_returncode()
        with tempfile.TemporaryDirectory(prefix=name + '-', dir=OUTPUT) as temporary:
            case = Path(temporary)
            (case / 'player').mkdir()
            (case / 'internal').mkdir()
            run = subprocess.run([str(exe)], env=env, cwd=case / 'player',
                                 capture_output=True, timeout=30)
            (OUTPUT / (name + '.log')).write_bytes(run.stdout + run.stderr)
            print(run.stdout.decode(errors='replace'), end='')
            if run.returncode:
                print(run.stderr.decode(errors='replace'), end='')
            run.check_returncode()
        records.append({'test': name, 'passed': True,
                        'mode': 'old SDL prefix failure reproduced' if reference else 'actual Android helper and both callers'})
    (OUTPUT / 'results.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    print('Logs:', OUTPUT)
    print('Host model only: Android device/game execution is not covered.')


if __name__ == '__main__':
    main()
