#!/usr/bin/env python3
"""MHGU: make the game code for the app from your own copy of the game.

    python build_game.py <folder with exefs and romfs>

The app itself holds no game code. This tool translates your exefs into libmhgugame.so (and libmhgugame.id) and leaves
them in that same folder, ready to copy to the phone. It reads your files only: nothing is sent anywhere, and the only
download is Google's Android NDK (the compiler, about 1.8 GB, kept in ~/.mhgu-tool) when you don't have it.

Options: --ndk <path>  an NDK of your own   --jobs <n>  compile with n jobs   --keep  keep the work folder
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
NDK_RELEASE = 'r27'
NDK_REVISION = '27.0.12077973'
API = 29
BUILD_ID = bytes.fromhex('FB08F1D20FD1204F')  # version 1.4.0's main
# (exactly native/CMakeLists.txt's for this library, with the NDK's own)
CFLAGS = ['-DANDROID', '-fdata-sections', '-ffunction-sections', '-funwind-tables', '-no-canonical-prefixes', '-DNDEBUG',
          '-fPIC', '-O2', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno', '-fvisibility=hidden',
          '-march=armv8.2-a', '-w', '-fno-stack-protector', '-U_FORTIFY_SOURCE', '-g0', '-fno-plt']


def say(text):
    print(text, flush=True)


def fail(text):
    say('\nERROR: ' + text)
    sys.exit(1)


def gamelib_id():
    path = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'assets', 'gamelib_id.txt')
    with open(path, encoding='ascii') as f:
        game_id = f.read().strip()
    if len(game_id) != 16 or any(char not in '0123456789abcdef' for char in game_id):
        fail(f'invalid GameLib ID in {path}')
    return game_id


def check_game(game):
    main = os.path.join(game, 'exefs', 'main')
    if not os.path.isfile(main):
        for sub in ('data',):
            if os.path.isfile(os.path.join(game, sub, 'exefs', 'main')):
                game = os.path.join(game, sub)
                main = os.path.join(game, 'exefs', 'main')
                break
    if not os.path.isfile(main):
        fail(f'there is no exefs/main in {game}. Point to the folder that holds exefs and romfs.')
    if not os.path.isfile(os.path.join(game, 'exefs', 'sdk')):
        fail('exefs/sdk is missing.')
    with open(main, 'rb') as f:
        head = f.read(0x48)
    if head[:4] != b'NSO0' or head[0x40:0x48] != BUILD_ID:
        fail('this exefs is not version 1.4.0. Use the one from the 1.4.0 update.')
    return game


def ensure_lz4():
    try:
        import lz4.block  # noqa: F401
    except ImportError:
        say('Installing the lz4 Python package...')
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', '--user', 'lz4'])


def host_tag():
    s = platform.system()
    return {'Windows': ('windows', 'windows-x86_64'), 'Linux': ('linux', 'linux-x86_64')}.get(s) or fail(f'{s} is not supported: use Windows or Linux.')


def find_ndk(given):
    names, tag = host_tag()[0], host_tag()[1]
    candidates = [given, os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')]
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk')
    candidates.append(os.path.join(sdk, 'ndk', NDK_REVISION))
    mine = os.path.join(os.path.expanduser('~'), '.mhgu-tool', f'android-ndk-{NDK_RELEASE}')
    candidates.append(mine)
    for c in candidates:
        if c and os.path.isdir(os.path.join(c, 'toolchains', 'llvm', 'prebuilt', tag)):
            return c
    url = f'https://dl.google.com/android/repository/android-ndk-{NDK_RELEASE}-{names}.zip'
    os.makedirs(os.path.dirname(mine), exist_ok=True)
    zip_path = mine + '.zip'
    say(f'Downloading the Android NDK {NDK_RELEASE} from Google (about 1.8 GB, only once)...')

    def progress(blocks, size, total):
        if total > 0 and blocks % 2000 == 0:
            say(f'  {min(100, blocks * size * 100 // total)}%')
    urllib.request.urlretrieve(url, zip_path, progress)
    say('Unpacking it...')
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(os.path.dirname(mine))
    os.remove(zip_path)
    if platform.system() != 'Windows':
        bin_dir = os.path.join(mine, 'toolchains', 'llvm', 'prebuilt', tag, 'bin')
        for f in os.listdir(bin_dir):
            os.chmod(os.path.join(bin_dir, f), 0o755)
    return mine


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    return r.returncode, r.stdout


def main():
    ap = argparse.ArgumentParser(description='Make the game code for the app from your own copy of the game.')
    ap.add_argument('game', help='the folder that holds exefs and romfs')
    ap.add_argument('--ndk', help='an Android NDK of your own')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--keep', action='store_true', help='keep the work folder')
    a = ap.parse_args()

    t0 = time.time()
    game = check_game(os.path.abspath(a.game))
    ensure_lz4()
    ndk = find_ndk(a.ndk)
    tag = host_tag()[1]
    exe = '.exe' if platform.system() == 'Windows' else ''
    bin_dir = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', tag, 'bin')
    clang = os.path.join(bin_dir, 'clang' + exe)
    strip = os.path.join(bin_dir, 'llvm-strip' + exe)
    if not os.path.isfile(clang):
        fail(f'no compiler in {bin_dir}')

    # room to work: the C it writes takes 300 MB, its objects a few hundred more
    work = tempfile.mkdtemp(prefix='mhgu_')
    if shutil.disk_usage(work).free < 3 << 30:
        fail('at least 3 GB of free disk space are needed.')
    gid = gamelib_id()
    say(f'Game code id {gid}. Working in {work}')

    say('Step 1 of 3: translating the game code...')
    gen = os.path.join(work, 'gen')
    hle = os.path.join(ROOT, 'runtime', 'hle', 'hle_symbols.txt')
    for cmd in ([sys.executable, 'gen.py', 'sdk', os.path.join(game, 'exefs', 'sdk'), '0x02400000', os.path.join(gen, 'sdk'), '--hle', hle],
                [sys.executable, 'gen.py', 'main', os.path.join(game, 'exefs', 'main'), '0x00200000', os.path.join(gen, 'main')]):
        code, out = run(cmd, cwd=TOOLS)
        if code:
            fail('the translation failed:\n' + out[-2000:])

    sources = []
    for sub in ('main', 'sdk'):
        d = os.path.join(gen, sub)
        sources += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.c')]
    sources.append(os.path.join(TOOLS, 'gamelib_entry.c'))
    say(f'Step 2 of 3: compiling {len(sources)} files with {a.jobs} jobs (this is the long part)...')
    objs_dir = os.path.join(work, 'obj')
    os.makedirs(objs_dir)
    target = f'--target=aarch64-linux-android{API}'
    done = [0]

    def compile_one(i_src):
        i, src = i_src
        obj = os.path.join(objs_dir, f'{i}.o')
        cmd = [clang, target] + CFLAGS + ['-I' + os.path.join(ROOT, 'runtime'), f'-DGAMELIB_ID="{gid}"', '-c', src, '-o', obj]
        code, out = run(cmd)
        if code:
            raise RuntimeError(f'{src}:\n{out[-1500:]}')
        done[0] += 1
        if done[0] % 20 == 0 or done[0] == len(sources):
            say(f'  {done[0]} of {len(sources)}')
        return obj

    with ThreadPoolExecutor(a.jobs) as pool:
        try:
            objs = list(pool.map(compile_one, enumerate(sources)))
        except RuntimeError as e:
            fail(f'the compiler failed on {e}')

    say('Step 3 of 3: linking...')
    # (only for its name: the app's own libmain.so answers these calls on the phone)
    stub_c = os.path.join(work, 'stub.c')
    open(stub_c, 'w').close()
    stub = os.path.join(work, 'libmain.so')
    code, out = run([clang, target, '-shared', '-Wl,-soname,libmain.so', stub_c, '-o', stub])
    if code:
        fail('linking failed:\n' + out[-1500:])
    rsp = os.path.join(work, 'objs.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"' + o.replace('\\', '/') + '"' for o in objs))
    lib = os.path.join(work, 'libmhgugame.so')
    code, out = run([clang, target, '-shared', '-fPIC', '-Wl,-soname,libmhgugame.so', '-Wl,--build-id=sha1', '-Wl,--no-rosegment',
                     '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-Wl,--allow-shlib-undefined', '-o', lib, '@' + rsp,
                     '-L' + work, '-lmain', '-latomic', '-lm'])
    if code:
        fail('linking failed:\n' + out[-1500:])
    run([strip, '--strip-unneeded', lib])

    shutil.copyfile(lib, os.path.join(game, 'libmhgugame.so'))
    with open(os.path.join(game, 'libmhgugame.id'), 'w') as f:
        f.write(gid)
    if a.keep:
        say(f'Work folder kept: {work}')
    else:
        shutil.rmtree(work, ignore_errors=True)
    size = os.path.getsize(os.path.join(game, 'libmhgugame.so')) >> 20
    say(f'\nDone in {(time.time() - t0) / 60:.0f} minutes: {size} MB.')
    say(f'Copy {game} to your phone and choose it in the app.')


if __name__ == '__main__':
    main()
