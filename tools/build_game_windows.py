#!/usr/bin/env python3
"""Build windows/mhgugame.dll from the player's own exefs.

The release app contains no game code. This tool translates a 1.4.0 exefs with
the runtime's tools and compiles the result for the Windows app. Nothing is
uploaded; only the Python packages lz4/capstone are installed if missing.

It needs a clang that targets x86_64-w64-windows-gnu (MSYS2's clang64 or
mingw64 toolchain). It also ships the import library for mhgurecomp.exe and
returns the DLL next to the game folder's exefs/romfs.

    py tools/build_game_windows.py <folder with exefs and romfs>
    py tools/build_game_windows.py --exe windows/mhgurecomp.exe <folder>
    py tools/build_game_windows.py --id 74b0adbd59f007af <folder>

Options:
  --clang PATH   clang.exe to use (default: PATH, then common MSYS2 paths)
  --exe PATH     mhgurecomp.exe whose GAMELIB_ID the DLL must match
  --id ID        explicit 16-hex GAMELIB_ID (overrides --exe detection)
  --jobs N       compile jobs (default: CPU count)
  --keep         keep the temporary translation/object folder
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = 'x86_64-w64-windows-gnu'
BUILD_ID = bytes.fromhex('FB08F1D20FD1204F')  # MHGU 1.4.0 main
ID_ASSET = os.path.join('android', 'app', 'src', 'main', 'assets', 'gamelib_id.txt')
# Must equal the Makefile's GENFLAGS for PLATFORM=windows.
FLAGS = ['-O1', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno',
         '-Wno-return-type', '-Wno-unused-label', '-Wno-parentheses-equality',
         '-Wno-unused-value', '-fvisibility=hidden', '-Iruntime',
         '-march=x86-64-v3', '-D_FILE_OFFSET_BITS=64']


def say(s):
    print(s, flush=True)


def die(s):
    say('\nERROR: ' + s)
    sys.exit(1)


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors='replace', **kw)
    return r.returncode, r.stdout


def find_clang(given):
    candidates = []
    if given:
        candidates.append(given)
    if os.environ.get('CLANG'):
        candidates.append(os.environ['CLANG'])
    w = shutil.which('clang')
    if w:
        candidates.append(w)
    for base in (r'C:\msys64', r'C:\msys32', os.path.expanduser('~/msys64')):
        for env in ('clang64', 'mingw64', 'ucrt64'):
            candidates.append(os.path.join(base, env, 'bin', 'clang.exe'))
    for c in candidates:
        if not c or not os.path.isfile(c):
            continue
        code, out = run([c, '--version'])
        if code == 0 and 'clang' in out.lower():
            return c
    die('no clang found: install MSYS2 clang64/mingw64, or pass --clang PATH')


def source_id():
    """The shared GAMELIB_ID asset (Makefile and native/build_native.sh use it too)."""
    p = os.path.join(ROOT, ID_ASSET)
    if not os.path.isfile(p):
        die(f'missing {p}; extract the whole builder archive')
    gid = open(p, encoding='ascii').read().strip()
    if not re.fullmatch(r'[0-9a-f]{16}', gid):
        die(f'invalid GAMELIB_ID in {p}: {gid!r}')
    return gid


def exe_ids(path):
    """The runtime stores its GAMELIB_ID as ASCII right after the table name."""
    data = open(path, 'rb').read()
    anchor = data.find(b'mhgugame_tables\x00')
    if anchor >= 0:
        m = re.search(rb'(?<![0-9A-Fa-f])[0-9A-Fa-f]{16}(?![0-9A-Fa-f])',
                      data[anchor:anchor + 256])
        if m:
            return [m.group().decode()]
    found = []
    for m in re.finditer(rb'(?<![0-9A-Fa-f])[0-9A-Fa-f]{16}(?![0-9A-Fa-f])', data):
        s = m.group().decode()
        if s not in found:
            found.append(s)
    return found


def check_game(game):
    if not os.path.isdir(game):
        die(f'no such folder: {game}')
    if not os.path.isfile(os.path.join(game, 'exefs', 'main')):
        for sub in ('data',):
            if os.path.isfile(os.path.join(game, sub, 'exefs', 'main')):
                game = os.path.join(game, sub)
                break
    main = os.path.join(game, 'exefs', 'main')
    if not os.path.isfile(main) or not os.path.isfile(os.path.join(game, 'exefs', 'sdk')):
        die(f'{game} must contain exefs/main and exefs/sdk')
    head = open(main, 'rb').read(0x48)
    if head[:4] != b'NSO0':
        die('exefs/main is not an NSO; use the files from a 1.4.0 dump')
    if head[0x40:0x48] != BUILD_ID:
        die('exefs/main is not MHGU 1.4.0 (build FB08F1D20FD1204F)')
    return os.path.abspath(game)


def deps():
    try:
        import lz4.block  # noqa: F401
    except ImportError:
        say('Installing lz4...')
        if subprocess.call([sys.executable, '-m', 'pip', 'install', '--user', 'lz4']) != 0:
            die('pip install lz4 failed')


def main():
    ap = argparse.ArgumentParser(description='Build mhgugame.dll from your own exefs.')
    ap.add_argument('game', help='folder containing exefs and romfs')
    ap.add_argument('--clang')
    ap.add_argument('--exe', help='mhgurecomp.exe to match')
    ap.add_argument('--id', help='explicit GAMELIB_ID (16 hex)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()

    t0 = time.time()
    game = check_game(os.path.abspath(a.game))
    clang = find_clang(a.clang)
    deps()
    sid = source_id()

    gid = a.id
    if gid and not re.fullmatch(r'[0-9A-Fa-f]{16}', gid):
        die('--id must be 16 hexadecimal characters')
    if not gid and a.exe:
        ids = exe_ids(a.exe)
        if sid in ids:
            gid = sid
        elif len(ids) == 1:
            gid = ids[0]
        else:
            die('cannot pick a GAMELIB_ID from the exe; pass --id (candidates: %s)' % ', '.join(ids))
    if not gid:
        gid = sid
    say(f'Game code id {gid}' + ('' if gid == sid else f' (build tree id is {sid})'))

    # Translation needs room for ~300 MB of C and its objects.
    work = tempfile.mkdtemp(prefix='mhgu_win_')
    if shutil.disk_usage(work).free < 3 << 30:
        die('needs at least 3 GB free space')
    gen = os.path.join(work, 'gen')
    hle = os.path.join(ROOT, 'runtime', 'hle', 'hle_symbols.txt')
    say(f'Working in {work}')
    say('Step 1 of 3: translating the game code...')
    for cmd in ([sys.executable, os.path.join(HERE, 'gen.py'), 'sdk',
                 os.path.join(game, 'exefs', 'sdk'), '0x02400000',
                 os.path.join(gen, 'sdk'), '--hle', hle],
                [sys.executable, os.path.join(HERE, 'gen.py'), 'main',
                 os.path.join(game, 'exefs', 'main'), '0x00200000',
                 os.path.join(gen, 'main')]):
        code, out = run(cmd, cwd=HERE)
        if code:
            die('translation failed:\n' + out[-3000:])

    sources = []
    for sub in ('main', 'sdk'):
        d = os.path.join(gen, sub)
        sources += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.c')]
    sources.append(os.path.join(HERE, 'gamelib_entry.c'))
    say(f'Step 2 of 3: compiling {len(sources)} files with {a.jobs} jobs...')
    objdir = os.path.join(work, 'obj')
    os.makedirs(objdir)
    done = [0]

    def compile_one(item):
        i, src = item
        obj = os.path.join(objdir, f'{i}.o')
        cmd = [clang, '--target=' + TARGET] + FLAGS + [
            '-I' + os.path.join(ROOT, 'runtime'),
            '-DGAMELIB_ID="' + gid + '"', '-c', src, '-o', obj]
        code, out = run(cmd)
        if code:
            raise RuntimeError(f'{src}:\n{out[-2000:]}')
        done[0] += 1
        if done[0] % 20 == 0 or done[0] == len(sources):
            say(f'  {done[0]} of {len(sources)}')
        return obj

    with ThreadPoolExecutor(a.jobs) as pool:
        try:
            objs = list(pool.map(compile_one, enumerate(sources)))
        except RuntimeError as e:
            die(f'compile failed:\n{e}')

    stubs = os.path.join(work, 'stubs.o')
    code, out = run([clang, '--target=' + TARGET, '-c',
                     os.path.join(HERE, 'windows', 'stubs.c'), '-o', stubs])
    if code:
        die('could not compile stubs.c:\n' + out)
    implib = os.path.join(HERE, 'windows', 'libmhgurecomp.dll.a')
    if not os.path.isfile(implib):
        die('missing windows/libmhgurecomp.dll.a in the builder archive')

    say('Step 3 of 3: linking mhgugame.dll...')
    outdll = os.path.join(game, 'mhgugame.dll')
    code, out = run([clang, '--target=' + TARGET, '-fuse-ld=lld', '-shared',
                     '-static-libgcc', '-o', outdll] + objs + [stubs, implib])
    if code:
        die('link failed:\n' + out[-3000:])
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        say(f'Work folder kept: {work}')
    with open(os.path.join(game, 'mhgugame.id'), 'w') as f:
        f.write(gid)
    size = os.path.getsize(outdll) >> 20
    say(f'\nDone in {(time.time() - t0) / 60:.1f} min: {outdll} ({size} MB).')
    say(f'ID {gid} written to {os.path.join(game, "mhgugame.id")}.')
    say('Put mhgugame.dll next to mhgurecomp.exe (the app loads it from its own folder).')


if __name__ == '__main__':
    main()
