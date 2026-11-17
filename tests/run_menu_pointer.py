"""Exercise production menu-pointer hooks and renderer mapping without a game or device."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(project)))
output = Path(os.environ.get('MHGU_MENU_POINTER_OUTPUT',
                             str(workspace / 'analysis/relink-menu-pointer-build')))
output.mkdir(parents=True, exist_ok=True)
inputs = ('runtime/hle/menuptr.c', 'runtime/hle/menuptr.h', 'runtime/hle/menuptr_table.h',
          'runtime/menulearn.c', 'runtime/hle.h', 'runtime/rt.h', 'runtime/cpu.h',
          'runtime/config.h', 'runtime/platform.h',
          'tests/test_menu_pointer.c', 'tests/run_menu_pointer.py')


def snapshot():
    return {name: hashlib.sha256((project / name).read_bytes()).hexdigest() for name in inputs}


before = snapshot()
record = dict(passed=False, source_sha256=before, source_unchanged=False,
              pass_groups=0, checks=[], device_tested=False,
              limits=['Native cursor steps and held/pressed Combine behavior use synthetic guest-memory fixtures.',
                      'No game or physical Android device is run.'])
try:
    vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True,
                           text=True, check=True, timeout=60)
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key] = value
    for key in ('MHGU_MENUPTR_NO_BUILTIN', 'MHGU_MENUPTR_NO_SHIPPED'):
        env.pop(key, None)
    compiler = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    exe = output / 'test-menu-pointer.exe'
    compiled = subprocess.run([
        str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
        '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE',
        '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-unused-parameter',
        '-I' + str(project / 'tests/host_compat'), '-I' + str(workspace / 'analysis/host-test-deps'),
        '-I' + str(project / 'runtime'), str(project / 'tests/test_menu_pointer.c'),
        str(project / 'runtime/menulearn.c'), '-o', str(exe),
    ], env=env, cwd=output, capture_output=True, timeout=120)
    (output / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    record['checks'].append(dict(name='compile', exit_code=compiled.returncode))
    if compiled.returncode:
        print((compiled.stdout + compiled.stderr).decode('utf-8', errors='replace'), end='')
    compiled.check_returncode()
    for case in ('known', 'select', 'nudge', 'unknown', 'expiry', 'mapping', 'mix', 'context',
                 'mix-repeat', 'mix-guards', 'mix-auto', 'mix-auto-limits', 'mix-auto-cancel',
                 'mix-pause', 'mix-pause-guards', 'mix-pause-mapping', 'mix-visibility'):
        with tempfile.TemporaryDirectory(prefix='menu-pointer-', dir=output) as scratch:
            result = subprocess.run([str(exe), case], env=env, cwd=scratch,
                                    capture_output=True, timeout=30)
        log = result.stdout + result.stderr
        (output / (case + '.log')).write_bytes(log)
        record['checks'].append(dict(name=case, exit_code=result.returncode))
        record['pass_groups'] += sum(line.startswith(b'PASS:') for line in log.splitlines())
        print(log.decode('utf-8', errors='replace'), end='')
        result.check_returncode()
    record['passed'] = True
finally:
    record['source_unchanged'] = before == snapshot()
    if not record['source_unchanged']:
        record['passed'] = False
    (output / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
    assert record['source_unchanged'], 'Source changed during the real menu-pointer tests'
print('Log:', output / 'result.json')
