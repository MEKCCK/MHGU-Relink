"""Build the ranged-input regression with the same installed host toolchain as touch tests."""
from pathlib import Path
import os
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis' / 'host-test-deps'
output = Path(os.environ.get('MHGU_RANGED_INPUT_OUTPUT', str(workspace / 'analysis' / 'relink-ranged-input-build')))
output.mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ.get('ANDROID_NDK_HOME', r'C:\Users\Public\Android\Sdk\ndk\30.0.16248370')) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
exe = output / 'test-ranged-input.exe'
subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
                '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-Wno-deprecated-declarations',
                '-I' + str(deps), '-I' + str(project / 'runtime'),
                str(project / 'tests' / 'test_ranged_input.c'), '-o', str(exe)],
               env=env, cwd=output, check=True, timeout=120)
result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=30)
(output / 'ranged-input-regression.log').write_bytes(result.stdout + result.stderr)
print(result.stdout.decode('utf-8', errors='replace'), end='')
print(result.stderr.decode('utf-8', errors='replace'), end='')
print('Log:', output / 'ranged-input-regression.log')
result.check_returncode()
subprocess.run([sys.executable, str(project / 'tests' / 'test_no_gyro.py')], check=True, timeout=30)
