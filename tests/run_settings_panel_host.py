"""Build the real settings-panel ImGui loop; run only its focused --touch-presets suite and host previews."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis/host-test-deps'
output = workspace / 'analysis/relink-settings-panel-build'
output.mkdir(parents=True, exist_ok=True)
(output / 'obj/settings-test').mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
toolchain = Path(os.environ.get('ANDROID_NDK_HOME', r'C:\Users\Public\Android\Sdk\ndk\30.0.16248370')) / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
sdl = deps / 'SDL3-3.4.18'
imgui = project / 'third_party/cimgui/imgui'
common = ['--target=x86_64-pc-windows-msvc', '-g', '-gcodeview', '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE',
          '-DW_OK=2', '-Dstrtok_r=strtok_s', '-Wno-deprecated-declarations', '-Wno-microsoft-goto',
          '-I' + str(deps), '-I' + str(project / 'runtime'), '-I' + str(imgui), '-I' + str(sdl / 'include')]
objects = []
for name in ('config', 'config_store', 'actions', 'modorder', 'arcmerge'):
    obj = output / (name + '.obj')
    command = [str(toolchain / 'clang.exe'), '-std=gnu11'] + common + [
        '-c', str(project / 'runtime' / (name + '.c')), '-o', str(obj)]
    build = subprocess.run(command, env=env, cwd=output, capture_output=True, timeout=120)
    log = build.stdout + build.stderr
    (output / (name + '-build.log')).write_bytes(log)
    print(log.decode('utf-8', errors='replace'), end='')
    build.check_returncode()
    objects.append(str(obj))
exe = output / 'test-settings-panel.exe'
sources = [project / 'tests/test_settings_panel.cpp', project / 'runtime/settings_panel.cpp',
           project / 'runtime/binding_view.cpp'] + [imgui / name for name in
           ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')]
command = [str(toolchain / 'clang++.exe'), '-fuse-ld=link', '-Wl,/debug', '-std=c++17'] + common + [str(p) for p in sources] + objects + [
    str(sdl / 'lib/x64/SDL3.lib'), '-o', str(exe)]
build = subprocess.run(command, env=env, cwd=output, capture_output=True, timeout=180)
log = build.stdout + build.stderr
(output / 'settings-panel-build.log').write_bytes(log)
print(log.decode('utf-8', errors='replace'), end='')
build.check_returncode()
shutil.copy2(sdl / 'lib/x64/SDL3.dll', output / 'SDL3.dll')
result = subprocess.run([str(exe), '--touch-presets'], env=env, cwd=output, capture_output=True, timeout=90)
log = result.stdout + result.stderr
(output / 'settings-panel-touch-presets.log').write_bytes(log)
print(log.decode('utf-8', errors='replace'), end='')
result.check_returncode()
print('PASS: focused --touch-presets suite only; real settings_panel.cpp / ImGui loop with host-side touch API stubs.')
print('This runner does not run the legacy no-argument suite, an APK, or the game. Logs and host previews:', output)
