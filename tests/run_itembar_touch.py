"""Run the actual production touch-owned item queue with synthetic guest memory."""
from pathlib import Path
import os
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件')
deps = workspace / 'analysis' / 'host-test-deps'
output = workspace / 'analysis' / 'relink-itembar-touch-build'
output.mkdir(parents=True, exist_ok=True)
vcvars = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True,
                       text=True, check=True, timeout=60)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(r'C:\Users\a9726\AppData\Local\Android\Sdk\ndk\30.0.16248370\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe')
exe = output / 'test-itembar-touch.exe'
build = subprocess.run([
    str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
    '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-Wno-deprecated-declarations',
    '-Wall', '-Wextra', '-Werror', '-I' + str(deps), '-I' + str(project / 'runtime'),
    str(project / 'tests' / 'test_itembar_touch.c'), '-o', str(exe),
], env=env, cwd=output, capture_output=True, timeout=120)
(output / 'itembar-touch-build.log').write_bytes(build.stdout + build.stderr)
print(build.stdout.decode('utf-8', errors='replace'), end='')
print(build.stderr.decode('utf-8', errors='replace'), end='')
build.check_returncode()
result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=30)
(output / 'itembar-touch-regression.log').write_bytes(result.stdout + result.stderr)
print(result.stdout.decode('utf-8', errors='replace'), end='')
print(result.stderr.decode('utf-8', errors='replace'), end='')
print('Log:', output / 'itembar-touch-regression.log')
result.check_returncode()
