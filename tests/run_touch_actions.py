"""Build touch/action integration and existing action regressions with installed host dependencies."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis/host-test-deps'
output = Path(os.environ.get('MHGU_TOUCH_ACTIONS_OUTPUT', workspace / 'analysis/relink-touch-actions-build'))
output.mkdir(parents=True, exist_ok=True)
(output / 'obj/settings-test').mkdir(parents=True, exist_ok=True)
vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value

host = (project / 'runtime/host_input.c').read_text(encoding='utf-8')
start = host.index('static bool mix_input_cancel(')
end = host.index('\nvoid host_input_update(', start)
touch_start = host.index('    float tdx = 0, tdy = 0;', end)
touch_end = host.index('    if (tdx != 0 || tdy != 0)', touch_start)
cancel_start = host.index('    bool mix_cancel = mix_input_cancel(', touch_end)
cancel_end = host.index('\n', cancel_start)
call_start = host.index('    b = act_update(b, ', touch_end)
call_end = host.index('\n    input_set(', call_start)
focus_start = host.index('    case SDL_EVENT_WINDOW_FOCUS_LOST:', host.index('void host_input_event('))
focus_end = host.index('\n        break;', focus_start)
focus_cancel = next(line for line in host[focus_start:focus_end].splitlines() if line.strip() == 'menuptr_mix_cancel();')
(output / 'host_touch_actions_under_test.h').write_text(
    host[start:end] + '\nstatic void host_touch_focus_lost(void) {\n' + focus_cancel + '\n}\n'
    'static void host_touch_sample(uint64_t physical_b, uint64_t scripted_b, uint64_t allowed, bool menu, bool typing) {\n'
    '    uint64_t b = physical_b | scripted_b, script_b = scripted_b;\n'
    '    int lx = requested_axes[0], ly = requested_axes[1], rx = requested_axes[2], ry = requested_axes[3];\n' + host[touch_start:touch_end] +
    host[cancel_start:cancel_end] + '\n' +
    '    b &= allowed;\n' + host[call_start:call_end] + '\n    host_buttons = b;\n}\n', encoding='utf-8')

mouse_state = host[host.index('#define MOUSE_FULL_SPEED '):host.index('// ---- the mouse for mods')]
mouse_start = host.index('static void mouse_update(')
mouse_end = host.index("\n// The game's rumble", mouse_start)
mods_start = host.index('static SDL_SpinLock mods_lock;')
mods_end = host.index('static SDL_AtomicInt mods_free;', mods_start) + len('static SDL_AtomicInt mods_free;')
camera_end = host.index('\n    mouse_update(&rx, &ry);', touch_start) + len('\n    mouse_update(&rx, &ry);')
lock_start = host.index('    b |= touch_lock_buttons(b, mouse_moved', camera_end)
lock_end = host.index('\n', lock_start)
(output / 'host_touch_camera_under_test.h').write_text(
    mouse_state + '\n' + host[mods_start:mods_end] + '\n' + host[mouse_start:mouse_end] +
    '\nstatic bool host_touch_camera_sample(bool typing, uint64_t *buttons, int *left_x, int *left_y, int *right_x, int *right_y) {\n'
    '    uint64_t b = *buttons;\n'
    '    int lx = *left_x, ly = *left_y, rx = *right_x, ry = *right_y;\n' + host[touch_start:camera_end] + '\n' +
    host[lock_start:lock_end] + '\n'
    '    (void)touch_b;\n'
    '    *buttons = b; *left_x = lx; *left_y = ly; *right_x = rx; *right_y = ry;\n'
    '    return mouse_moved;\n}\n', encoding='utf-8')

compiler = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
sdl = deps / 'SDL3-3.4.18'
common = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
          '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-DW_OK=2',
          '-Dstrtok_r=strtok_s', '-Wno-deprecated-declarations',
          '-I' + str(project / 'tests/host_compat'), '-I' + str(deps), '-I' + str(output),
          '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include')]
sources = [str(project / 'runtime' / name) for name in ('actions.c', 'config.c', 'config_store.c')]
shutil.copy2(sdl / 'lib/x64/SDL3.dll', output / 'SDL3.dll')
for name in ('test_touch_actions', 'test_actions', 'test_touch_camera'):
    exe = output / (name + '.exe')
    production = [] if name == 'test_touch_camera' else sources + [str(sdl / 'lib/x64/SDL3.lib')]
    build = subprocess.run(common + [str(project / 'tests' / (name + '.c'))] + production +
                           ['-o', str(exe)], env=env, cwd=output, capture_output=True, timeout=120)
    (output / (name + '-build.log')).write_bytes(build.stdout + build.stderr)
    if build.returncode:
        print((build.stdout + build.stderr).decode('utf-8', errors='replace'), end='')
    build.check_returncode()
    result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=60)
    (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    print(result.stdout.decode('utf-8', errors='replace'), end='')
    print(result.stderr.decode('utf-8', errors='replace'), end='')
    result.check_returncode()
print('Logs:', output)
