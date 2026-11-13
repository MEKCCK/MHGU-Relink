"""Build the ranged-input regression with the same installed host toolchain as touch tests."""
from pathlib import Path
import os
import subprocess
import shutil

project = Path(__file__).resolve().parents[1]
workspace = Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件')
deps = workspace / 'analysis' / 'host-test-deps'
output = workspace / 'analysis' / 'relink-ranged-input-build'
output.mkdir(parents=True, exist_ok=True)
vcvars = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(r'C:\Users\a9726\AppData\Local\Android\Sdk\ndk\30.0.16248370\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe')
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

sdl = deps / 'SDL3-3.4.18'
exe = output / 'test-gyro-sources.exe'
host = (project / 'runtime' / 'host_input.c').read_text(encoding='utf-8')
start = host.index('// Only the selected controller\'s gyro is read;')
end = host.index('// The game\'s rumble (input_rumble)', start)
(output / 'host_gyro_under_test.h').write_text(host[start:end], encoding='utf-8')
subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
                '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-Wno-deprecated-declarations',
                '-I' + str(deps), '-I' + str(output), '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include'),
                str(project / 'tests' / 'test_gyro_sources.c'), str(sdl / 'lib' / 'x64' / 'SDL3.lib'), '-o', str(exe)],
               env=env, cwd=output, check=True, timeout=120)
shutil.copy2(sdl / 'lib' / 'x64' / 'SDL3.dll', output / 'SDL3.dll')
result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=30)
(output / 'gyro-sources-regression.log').write_bytes(result.stdout + result.stderr)
print(result.stdout.decode('utf-8', errors='replace'), end='')
print(result.stderr.decode('utf-8', errors='replace'), end='')
print('Log:', output / 'gyro-sources-regression.log')
result.check_returncode()
