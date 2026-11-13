"""Build and run settings persistence and translation checks with the installed host toolchain."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis' / 'host-test-deps'
output = workspace / 'analysis' / 'relink-compatible-build' / 'host-settings'
output.mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ.get('ANDROID_NDK_HOME', r'C:\Users\Public\Android\Sdk\ndk\30.0.16248370')) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
sdl = deps / 'SDL3-3.4.18'
exe = output / 'test-settings.exe'
args = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=c17',
        '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-DW_OK=2', '-Wno-deprecated-declarations',
        '-I' + str(deps), '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include'),
        str(project / 'tests' / 'test_settings.c'), str(project / 'runtime' / 'config.c'),
        str(project / 'runtime' / 'config_store.c'), str(sdl / 'lib' / 'x64' / 'SDL3.lib'), '-o', str(exe)]
build = subprocess.run(args, env=env, cwd=output, capture_output=True, timeout=60)
(output / 'settings-build.log').write_bytes(build.stdout + build.stderr)
print((build.stdout + build.stderr).decode('utf-8', errors='replace'), end='')
build.check_returncode()
shutil.copy2(sdl / 'lib' / 'x64' / 'SDL3.dll', output / 'SDL3.dll')
checks = [('settings-regression', [str(exe), 'settings-regression.ini']),
          ('settings-locale', [sys.executable, str(project / 'tests' / 'test_settings_locale.py')])]
for name, command in checks:
    result = subprocess.run(command, env=env, cwd=output, capture_output=True, timeout=30)
    log = result.stdout + result.stderr
    (output / (name + '.log')).write_bytes(log)
    print(log.decode('utf-8', errors='replace'), end='')
    result.check_returncode()
print('PASS: settings persistence and locale checks. Logs:', output)
