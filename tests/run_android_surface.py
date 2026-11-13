"""Exercise production Android surface recovery with deterministic WSI failures."""
from pathlib import Path
import json
import os
import re
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
output = workspace / 'analysis/relink-0.2.2-20261113/host-surface'
output.mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/30.0.16248370'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'

def function(source, name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\)\s*\{', source, re.M)
    assert match, name
    depth = 0
    for end in range(source.index('{', match.start()), len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[match.start():end+1] + '\n'
    raise AssertionError(name)

records = []
for reference in (True, False):
    source = subprocess.check_output(['git', 'show', '35a8c23:runtime/nvn/vulkan/device.c'], cwd=project).decode() if reference else (project / 'runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
    code = ''.join(function(source, name) for name in ('surface_destroy', 'surface_ready', 'android_window_check'))
    # Keep lifecycle/acquire unchanged; omit only render-command recording after a successful acquire.
    code += function(source, 'frame_begin').split('    cur_frame.acquired =')[0] + '    return true;\n}\n'
    (output / 'surface_under_test.h').write_text(code, encoding='utf-8')
    name = 'surface-' + ('baseline' if reference else 'fixed')
    exe = output / (name + '.exe')
    build = subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
        '-D_CRT_SECURE_NO_WARNINGS', '-D__ANDROID__', '-DREFERENCE_BUILD=' + str(int(reference)),
        '-I' + str(output), str(project / 'tests/test_android_surface.c'), '-o', str(exe)], env=env, capture_output=True, timeout=120)
    (output / (name + '-build.log')).write_bytes(build.stdout + build.stderr)
    if build.returncode:
        print((build.stdout + build.stderr).decode(errors='replace'))
    build.check_returncode()
    run = subprocess.run([str(exe)], env=env, capture_output=True, timeout=30)
    (output / (name + '.log')).write_bytes(run.stdout + run.stderr)
    print((run.stdout + run.stderr).decode(errors='replace'), end='')
    run.check_returncode()
    records.append({'test': name, 'passed': True, 'reference': reference})
(output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
