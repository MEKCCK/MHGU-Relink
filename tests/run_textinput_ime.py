"""Build and run the real Android IME state transitions on the host."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis' / 'host-test-deps'
output = workspace / 'analysis' / 'relink-touch-build' / 'host-ime'
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
exe = output / 'test-textinput-ime.exe'
args = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=c11',
        '-D_CRT_SECURE_NO_WARNINGS', '-Wno-ignored-attributes',
        '-I' + str(deps), '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include'),
        str(project / 'tests' / 'test_textinput_ime.c'), str(sdl / 'lib' / 'x64' / 'SDL3.lib'), '-o', str(exe)]
subprocess.run(args, env=env, cwd=output, check=True, timeout=60)
shutil.copy2(sdl / 'lib' / 'x64' / 'SDL3.dll', output / 'SDL3.dll')
result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=20)
(output / 'textinput-ime-regression.log').write_bytes(result.stdout + result.stderr)
print(result.stdout.decode('utf-8', errors='replace'), end='')
print(result.stderr.decode('utf-8', errors='replace'), end='')
result.check_returncode()
