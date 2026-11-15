"""Compile the checkout's actual normalized touch -> menu-coordinate bridge with small window spies."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(project)))
output = Path(os.environ.get('MHGU_TOUCH_MENU_POINT_OUTPUT',
                             str(workspace / 'analysis/relink-touch-menu-point-build')))
output.mkdir(parents=True, exist_ok=True)
inputs = ('runtime/host_input.c', 'tests/test_touch_menu_point.c', 'tests/run_touch_menu_point.py')

def snapshot():
    return {name: hashlib.sha256((project / name).read_bytes()).hexdigest() for name in inputs}

before = snapshot()
host = (project / 'runtime/host_input.c').read_text(encoding='utf-8')
start = host.index('static bool menu_ptr_geometry(SDL_Window *w, float *left, float *top, float *sx, float *sy) {')
end = host.index('static float menu_ptr_gx', start)
fragment = host[start:end]
assert 'bool host_input_menu_point(' in fragment, 'Touch-menu bridge is absent from the current source'
(output / 'host_touch_menu_point_under_test.h').write_text(fragment, encoding='utf-8')

vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ.get('ANDROID_NDK_HOME', r'C:\Users\Public\Android\Sdk\ndk\30.0.16248370')) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
exe = output / 'test-touch-menu-point.exe'
compiled = subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
                           '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(output),
                           str(project / 'tests/test_touch_menu_point.c'), '-o', str(exe)],
                          env=env, cwd=output, capture_output=True, timeout=120)
(output / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
if compiled.returncode:
    print((compiled.stdout + compiled.stderr).decode('utf-8', errors='replace'))
compiled.check_returncode()
result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=30)
log = result.stdout + result.stderr
(output / 'touch-menu-point-regression.log').write_bytes(log)
assert before == snapshot(), 'Source changed during the menu-coordinate test'
record = dict(passed=result.returncode == 0, exit_code=result.returncode, source_sha256=before,
              source_unchanged=True, pass_groups=sum(line.startswith(b'PASS:') for line in log.splitlines()),
              device_tested=False)
(output / 'result.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
print(log.decode('utf-8', errors='replace'), end='')
print('Log:', output / 'touch-menu-point-regression.log')
result.check_returncode()
