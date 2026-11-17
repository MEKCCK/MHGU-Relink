"""Build the real settings-panel ImGui loop; run its focused touch and Forge suites and host previews."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
version = (project / 'VERSION').read_text(encoding='utf-8').strip()
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis/host-test-deps'
output = Path(os.environ.get('MHGU_SETTINGS_PANEL_OUTPUT', workspace / 'analysis/relink-settings-panel-build'))
output.mkdir(parents=True, exist_ok=True)
(output / 'obj/settings-test').mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
toolchain = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
sdl = deps / 'SDL3-3.4.18'
imgui = project / 'third_party/cimgui/imgui'
common = ['--target=x86_64-pc-windows-msvc', '-g', '-gcodeview', '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE',
          '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', '-DMHGU_TOUCH_ICON_PICKER_TEST', f'-DMHGU_VERSION="{version}"',
          '-DW_OK=2', '-Dstrtok_r=strtok_s', '-Wno-deprecated-declarations', '-Wno-microsoft-goto',
          '-I' + str(project / 'tests/host_compat'), '-I' + str(deps), '-I' + str(project / 'runtime'),
          '-I' + str(imgui), '-I' + str(sdl / 'include')]
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
for suite in ('touch-templates', 'forge-menu'):
    result = subprocess.run([str(exe), '--' + suite], env=env, cwd=output, capture_output=True, timeout=90)
    log = result.stdout + result.stderr
    (output / ('settings-panel-' + suite + '.log')).write_bytes(log)
    print(log.decode('utf-8', errors='replace'), end='')
    result.check_returncode()
print('PASS: focused --touch-templates and --forge-menu suites; real settings_panel.cpp / ImGui loop with host-side touch API stubs.')
print('This runner does not run the legacy no-argument suite, an APK, or the game. Logs and host previews:', output)
