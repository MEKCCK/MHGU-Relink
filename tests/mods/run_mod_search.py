"""Compile the production directory search and exercise it against real folders on Windows."""

import ctypes
from _ctypes import FreeLibrary
from contextlib import contextmanager
import os
from pathlib import Path
import re
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[2]
CLANG = (Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk/ndk/27.0.12077973"
         / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe")
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"


HARNESS = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>
#define strcasecmp _stricmp

struct dirent { char d_name[MAX_PATH]; };
typedef struct { HANDLE handle; WIN32_FIND_DATAA data; bool first; struct dirent entry; } DIR;

static DIR *opendir(const char *path) {
    char pattern[2048];
    if (snprintf(pattern, sizeof pattern, "%s/*", path) >= sizeof pattern) return NULL;
    DIR *d = malloc(sizeof *d);
    if (!d) return NULL;
    d->handle = FindFirstFileA(pattern, &d->data);
    if (d->handle == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = true;
    return d;
}
static struct dirent *readdir(DIR *d) {
    if (d->first) d->first = false;
    else if (!FindNextFileA(d->handle, &d->data)) return NULL;
    snprintf(d->entry.d_name, sizeof d->entry.d_name, "%s", d->data.cFileName);
    return &d->entry;
}
static void closedir(DIR *d) { FindClose(d->handle); free(d); }
static bool is_dir(const char *path) {
    DWORD flags = GetFileAttributesA(path);
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY);
}
"""


@contextmanager
def compile_search(directory):
    source = (PROJECT / "runtime/modorder.c").read_text(encoding="utf-8")
    start = source.index("bool mods_find_dir(")
    end = source.index("\nbool mods_romfs_of(", start)
    harness = directory / "search.c"
    harness.write_text(HARNESS + source[start:end], encoding="utf-8")
    env = os.environ.copy()
    setup = subprocess.run(f'call "{VCVARS}" >nul && set', shell=True, check=True,
                           capture_output=True, text=True)
    for line in setup.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key] = value
    library = directory / "search.dll"
    args = [str(CLANG), "--target=x86_64-pc-windows-msvc", "-fuse-ld=link", "-std=gnu17",
            "-D_CRT_SECURE_NO_WARNINGS", "-shared", str(harness), "-o", str(library),
            "-Xlinker", "/export:mods_find_dir"]
    subprocess.run(args, env=env, cwd=directory, check=True)
    loaded = ctypes.CDLL(str(library))
    try:
        yield loaded
    finally:
        FreeLibrary(loaded._handle)


def main():
    header = (PROJECT / "runtime/modorder.h").read_text(encoding="utf-8")
    search_depth = int(re.search(r"#define MODS_SEARCH_DEPTH (\d+)", header)[1])
    assert search_depth == 8
    with tempfile.TemporaryDirectory(prefix="mhgu_mod_search_") as tmp, compile_search(Path(tmp)) as library:
        root = Path(tmp)
        search = library.mods_find_dir
        search.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
                           ctypes.c_void_p, ctypes.c_size_t]
        search.restype = ctypes.c_bool

        def run(path, wanted="romfs", depth=search_depth, size=2048):
            out = ctypes.create_string_buffer(b"!" * (size + 32), size + 32)
            found = search(os.fsencode(path), wanted.encode(), depth, ctypes.addressof(out) + 16, size)
            assert out.raw[:16] == b"!" * 16 and out.raw[-16:] == b"!" * 16, "buffer guard overwritten"
            if not found:
                assert out.raw == b"!" * (size + 32), "failure wrote to output buffer"
                return None
            value = out.raw[16:16 + size]
            assert b"\0" in value, "output missing terminator"
            return os.fsdecode(value.split(b"\0", 1)[0]).replace("\\", "/")

        def make(base, *parts):
            path = base.joinpath(*parts)
            path.mkdir(parents=True, exist_ok=True)
            return str(path).replace("\\", "/")

        depth8 = root / "depth8"
        expected = make(depth8, *[f"w{i}" for i in range(7)], "romfs")
        assert run(depth8) == expected
        assert run(depth8, depth=7) is None
        depth9 = root / "depth9"
        make(depth9, *[f"w{i}" for i in range(8)], "romfs")
        assert run(depth9) is None

        shallow = root / "shallow"
        make(shallow, "a", "b", "romfs")
        expected = make(shallow, "z", "RoMfS")
        assert run(shallow) == expected

        siblings = root / "siblings"
        for index in range(160):
            make(siblings, f"wrapper{index:03}")
        expected = make(siblings, "wrapper159", "romfs")
        assert run(siblings) == expected

        skipped = root / "skipped"
        for name in ("__MACOSX", ".hidden", "exefs", "nativeNX"):
            make(skipped, name, "romfs")
        assert run(skipped) is None
        expected = make(skipped, "valid", "romfs")
        assert run(skipped) == expected
        nested_resource = root / "nested_resource"
        make(nested_resource, "romfs", "exefs")
        assert run(nested_resource, wanted="exefs") is None
        exact = root / "exact"
        expected = make(exact, "romfs")
        assert run(exact, size=len(expected)) is None
        assert run(exact, size=len(expected) + 1) == expected
        shorter = root / "shorter"
        make(shorter, "aaaaaaaa", "romfs")
        expected = make(shorter, "z", "romfs")
        assert run(shorter, size=len(expected) + 1) == expected

        assert run(shallow, size=3) is None
        assert run(shallow, size=0) is None
        assert run(shallow, depth=0) is None
        assert run(shallow, depth=-1) is None
        assert run(root / "missing") is None
        print("PASS production mod directory search: depth 8/9, shallow priority, 160 siblings, skipped resources, guarded buffers")


if __name__ == "__main__":
    main()
