"""Probe current common/template serializers and Android relative-path commits.

The production helper and serializer are extracted unchanged. Windows fixtures
model POSIX replacement and SDL's Android internal-storage prefix; no APK, game,
SDL DLL, or external source is used.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
WORKSPACE = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(PROJECT)))
OUTPUT = WORKSPACE / 'analysis/common-layout-templates-20261114/touch-storage'


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
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    assert(file); file << text; file.close(); assert(file);
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
    assert(fs::is_regular_file(from));
    if (fail_commit) { errno = EACCES; return -1; }
    return replace_file(from, to) ? 0 : -1;
}
static bool SDL_RenamePath(const char *from, const char *to) {
    ++sdl_calls;
    assert(!fs::path(from).is_absolute() && !fs::path(to).is_absolute());
    return replace_file(internal / from, internal / to);
}

// Geometry count is irrelevant to this storage probe. Keep the actual serializer.
static constexpr int C_LAID = 1, TZ_COUNT = 1;
static constexpr int TOUCH_COMBOS = 6, TOUCH_COMBO_NAME_MAX = 64, UNIVERSAL_LAYOUT = 19;
static const struct { const char *key; } DEFS[] = {{"test_button"}};
static const char *ZONE_KEY[] = {"test_zone"};
struct Layout {
    float x[C_LAID]{.25f}, y[C_LAID]{.5f}, scale[C_LAID]{1};
    float zx[TZ_COUNT]{12}, zy[TZ_COUNT]{24};
    int style = 1;
};
struct Config {
    char touch_weapon_combo[20][TOUCH_COMBOS][32]{};
    int touch_weapon_combo_pad[20][TOUCH_COMBOS]{};
    char touch_combo_name[TOUCH_COMBOS][TOUCH_COMBO_NAME_MAX]{};
};
static uint32_t config_combo_mask(const char *text) {
    assert(!strcmp(text, "X+A") || !strcmp(text, "L+R"));
    return !strcmp(text, "X+A") ? 5 : 192;
}
// System headers use Windows. Only extracted production functions see Android.
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
static void serialized_data(const std::string &text, bool custom) {
    assert(text.find("layout_version 1\n") == 0);
    assert(text.find("test_zone 12.0 24.0 1\n") != std::string::npos);
    assert(text.find("attack_style 1 0 1\n") != std::string::npos);
    assert(text.find("weapon_profile") == std::string::npos);
    if (!custom) {
        assert(text.find("template_name") == std::string::npos);
        assert(text.find("combo_") == std::string::npos);
        return;
    }
    assert(text.find("template_name \xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89\n") != std::string::npos);
    for (int i = 0; i < TOUCH_COMBOS; ++i) {
        const std::string suffix = " " + std::to_string(i + 1) + " ";
        assert(text.find("combo_mask" + suffix + std::to_string(i % 2 ? 192 : 5) + "\n") != std::string::npos);
        assert(text.find("combo_pad" + suffix + std::to_string(i) + "\n") != std::string::npos);
        const std::string name = i == 5 ? "" : "\xe7\xbb\x84\xe5\x90\x88" + std::to_string(i + 1);
        assert(text.find("combo_name" + suffix + name + "\n") != std::string::npos);
    }
}
static void layout_saves(bool reference) {
    Config combos;
    for (int i = 0; i < TOUCH_COMBOS; ++i) {
        strcpy(combos.touch_weapon_combo[UNIVERSAL_LAYOUT][i], i % 2 ? "L+R" : "X+A");
        combos.touch_weapon_combo_pad[UNIVERSAL_LAYOUT][i] = i;
        if (i != 5) snprintf(combos.touch_combo_name[i], TOUCH_COMBO_NAME_MAX, "\xe7\xbb\x84\xe5\x90\x88%d", i + 1);
    }
    for (const char *path : {"touch_layout_common.txt", "touch_layout_common.txt.apply",
                             "touch_template_01.txt", "touch_template_20.txt"}) {
        const bool custom = strstr(path, "template") != nullptr;
        const char *name = custom ? "\xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89" : nullptr;
        const Config *config = custom ? &combos : nullptr;
        const std::string temp = std::string(path) + ".tmp";
        write_file(path, "old layout\n");
        write_file(internal / path, "internal sentinel\n");
        Layout layout;
        assert(save_layout_file(layout, path, name, config) == !reference);
        calls(reference); assert(!fs::exists(temp)); untouched_internal(path);
        if (reference) {
            assert(read_file(path) == "old layout\n");
            continue;
        }
        serialized_data(read_file(path), custom);
        assert(read_file(path).find("test_button 0.2500 0.5000 1.000\n") != std::string::npos);
        layout.x[0] = .75f;
        assert(save_layout_file(layout, path, name, config));
        calls(false);
        const std::string saved = read_file(path);
        serialized_data(saved, custom);
        assert(saved.find("test_button 0.7500 0.5000 1.000\n") != std::string::npos);
        fail_commit = true; layout.x[0] = .9f;
        assert(!save_layout_file(layout, path, name, config));
        calls(false); assert(read_file(path) == saved && !fs::exists(temp)); untouched_internal(path);
        fail_commit = false;
        assert(save_layout_file(layout, path, name, config));  // A later explicit save remains possible.
        calls(false); assert(!fs::exists(temp)); untouched_internal(path);
        serialized_data(read_file(path), custom);
        assert(read_file(path).find("test_button 0.9000 0.5000 1.000\n") != std::string::npos);
    }
    puts(reference ? "PASS baseline: Android-prefixed SDL rename cannot commit cwd temp files; all four old targets remain intact"
                   : "PASS fixed: common, staged common and template slots 1/20 serialize geometry plus six combo masks/pads/UTF-8 names; cwd replacement preserves prior files on failure and permits a later explicit save");
}
int main() {
    assert(_set_fmode(_O_BINARY) == 0);
    internal = fs::current_path().parent_path() / "internal";
    assert(fs::current_path().filename() == "player" && fs::is_directory(internal));
    layout_saves(REFERENCE_BUILD != 0);
    return 0;
}
'''


def main():
    source_path = PROJECT / 'runtime/touch.cpp'
    source = source_path.read_text(encoding='utf-8')
    helper = function(source, 'commit_layout_file')
    serializer = function(source, 'save_layout_file')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    vcvars = Path(os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'))
    setup = subprocess.run('call "' + str(vcvars) + '" >nul && set', shell=True,
                           capture_output=True, text=True, check=True)
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, separator, value = line.partition('=')
        if separator and key:
            env[key] = value
    ndk = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/27.0.12077973')))
    compiler = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    assert compiler.is_file(), compiler
    probe = OUTPUT / 'touch-storage-probe.cpp'
    baseline = 'bool commit_layout_file(const char *temp, const char *path) { return SDL_RenamePath(temp, path); }\n'
    probe.write_text(FIXTURE + '\n#if REFERENCE_BUILD\n' + baseline + '#else\n'
                     + helper + '#endif\n' + serializer + CHECKS, encoding='utf-8')
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
                        'mode': 'old SDL prefix failure reproduced' if reference else 'actual Android commit helper and common/template serializer'})
    result = {'checks': records, 'touch_cpp_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
              'test_script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'limitations': ['Host path model; no Android device/game execution',
                              'Config companion rollback is tested separately',
                              'Windows POSIX fixture alone retains bounded sharing-conflict retries; production helper has no retry']}
    (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('Logs:', OUTPUT)
    print('Host model only: Android device/game execution is not covered.')


if __name__ == '__main__':
    main()
