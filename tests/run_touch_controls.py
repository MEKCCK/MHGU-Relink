"""Build the SDL3 host regression using the workspace's installed dependencies."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis' / 'host-test-deps'
output = Path(os.environ.get('MHGU_TOUCH_HOST_OUTPUT', workspace / 'analysis/relink-touch-build/host-touch-compatible'))
output.mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
sdl = deps / 'SDL3-3.4.18'
imgui = project / 'third_party' / 'cimgui' / 'imgui'
exe = output / 'test-touch-controls.exe'
sha = output / 'sha256.obj'
subprocess.run([str(compiler.with_name('clang.exe')), '--target=x86_64-pc-windows-msvc', '-std=c11',
                '-D_CRT_SECURE_NO_WARNINGS', '-c', str(project / 'updater/sha256.c'), '-o', str(sha)],
               env=env, cwd=output, check=True, timeout=120)
args = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=c++17',
        '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH',
        '-Wno-deprecated-declarations',
        '-I' + str(deps), '-I' + str(project / 'runtime'), '-I' + str(imgui), '-I' + str(sdl / 'include'),
        str(project / 'tests' / 'test_touch_controls.cpp')]
args += [str(imgui / f) for f in ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp']]
args += [str(sha), str(sdl / 'lib' / 'x64' / 'SDL3.lib'), '-o', str(exe)]
subprocess.run(args, env=env, cwd=output, check=True, timeout=120)
shutil.copy2(sdl / 'lib' / 'x64' / 'SDL3.dll', output / 'SDL3.dll')
run_root = Path(os.environ.get('MHGU_HOST_TEST_RUN_ROOT', tempfile.gettempdir()))
run_root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='mhgu-touch-', dir=run_root) as temporary:
    print('Isolated runtime directory:', temporary)
    result = subprocess.run([str(exe)], env=env, cwd=temporary, capture_output=True, timeout=60)
    for path in Path(temporary).glob('*.png'):
        shutil.copy2(path, output / path.name)
(output / 'touch-controls-regression.log').write_bytes(result.stdout + result.stderr)
print(result.stdout.decode('utf-8', errors='replace'), end='')
if result.returncode:
    print(result.stderr.decode('utf-8', errors='replace'), end='')
print('Exit code:', result.returncode)
print('Log:', output / 'touch-controls-regression.log')
result.check_returncode()
