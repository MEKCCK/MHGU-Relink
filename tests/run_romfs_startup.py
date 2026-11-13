"""Check one-scan startup and optionally compare images/timings with an earlier Git revision."""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compare-ref')
args = parser.parse_args()
project = Path(__file__).resolve().parents[1]
workspace = Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件')
deps = workspace / 'analysis/host-test-deps'
output = workspace / 'analysis/startup-0.2.1-20261113/host-romfs'
output.mkdir(parents=True, exist_ok=True)
vcvars = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat')
setup = subprocess.run('call "' + str(vcvars) + '" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
# Force the existing serial fallback; this test has no concurrent file reads or archive merges.
(output / 'pthread.h').write_text('''#pragma once
typedef int pthread_t;
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static inline int pthread_create(pthread_t *t, const void *a, void *(*fn)(void *), void *p) { return -1; }
static inline int pthread_join(pthread_t t, void **p) { return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { return 0; }
''', encoding='utf-8')

def build(name, source=None):
    exe = output / (name + '.exe')
    command = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-O2', '-std=gnu11',
               '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-Wno-deprecated-declarations',
               '-I' + str(output), '-I' + str(deps), '-I' + str(project / 'runtime')]
    if source:
        command += ['-DTEST_PREPARE=0', '-DROMFS_SOURCE="' + source.as_posix() + '"']
    command += [str(project / 'tests/test_romfs_startup.c'), '-o', str(exe)]
    result = subprocess.run(command, env=env, capture_output=True, timeout=60)
    (output / (name + '-build.log')).write_bytes(result.stdout + result.stderr)
    if result.returncode:
        print((result.stdout + result.stderr).decode('utf-8', errors='replace'))
    result.check_returncode()
    return exe

versions = {'current': build('current')}
if args.compare_ref:
    baseline = output / 'baseline-romfs.c'
    baseline.write_bytes(subprocess.check_output(['git', 'show', args.compare_ref + ':runtime/romfs.c'], cwd=project))
    versions['baseline'] = build('baseline', baseline)
records = []
with tempfile.TemporaryDirectory(prefix='synthetic-', dir=output) as temporary:
    root = Path(temporary)
    base, first, last = [root / name for name in ('base', 'mod-first', 'mod-last')]
    for folder in (base, first, last):
        (folder / 'nativeNX').mkdir(parents=True)
    (base / 'nativeNX/common.bin').write_bytes(b'original')
    (first / 'nativeNX/common.bin').write_bytes(b'first')
    (last / 'nativeNX/common.bin').write_bytes(b'last')
    (first / 'nativeNX/new.bin').write_bytes(b'added by mod 1')
    (last / 'nativeNX/last.bin').write_bytes(b'added by mod 2')
    (base / 'nativeNX/.ignored').write_bytes(b'hidden')
    for name in ('a/one', 'a/two/deep', 'b/one', 'b/two', 'empty'):
        (base / name).mkdir(parents=True)
    for name in ('a/one/a.bin', 'a/one/b.bin', 'a/two/deep/a.bin', 'b/one/a.bin', 'b/two/a.bin'):
        (base / name).write_bytes(name.encode())
    count = 6

    def run(stage, mode):
        images = []
        for name, exe in versions.items():
            image = output / (stage + '-' + name + '.bin')
            result = subprocess.run([str(exe), str(base), str(first), str(last), str(image), str(count), mode],
                                    cwd=output, env=env, capture_output=True, timeout=120)
            log = (result.stdout + result.stderr).decode('utf-8', errors='replace')
            (output / (stage + '-' + name + '.log')).write_text(log, encoding='utf-8')
            if result.returncode:
                print(log)
            result.check_returncode()
            seconds = float(re.search(r'([0-9.]+) s to index', log)[1])
            records.append({'stage': stage, 'version': name, 'index_seconds': seconds, 'files': count})
            images.append(image.read_bytes())
            print(stage, name, result.stdout.decode().strip(), flush=True)
        assert all(value == images[0] for value in images), 'Resource image changed'
        return images[0]

    before = run('nested-overlays', 'cases')
    (base / 'b/one/a.bin').write_bytes(b'changed size between launches')
    (base / 'a/one/b.bin').unlink()
    count -= 1
    after = run('fresh-next-launch', 'cases')
    assert before != after, 'Changes between launches were lost'
    # One large folder exposes the previous quadratic sibling lookup.
    for i in range(35531 - count):
        (base / 'nativeNX' / ('file-%05d.bin' % i)).touch()
    count = 35531
    for repeat in range(3):
        run('large-' + str(repeat), 'benchmark')
(output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('PASS: base validation, one scan, nested siblings, mod order, DLC isolation, fresh launches and equivalent image bytes.')
