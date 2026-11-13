"""Build touch/action integration and existing action regressions with installed host dependencies."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
deps = workspace / 'analysis/host-test-deps'
output = workspace / 'analysis/relink-touch-actions-build'
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
start = host.index('static void act_update(')
end = host.index('\nvoid host_input_update(', start)
touch_start = host.index('    float tdx = 0, tdy = 0;', end)
touch_end = host.index('    if (tdx != 0 || tdy != 0)', touch_start)
call_start = host.index('    act_update(b, ', touch_end)
call_end = host.index('\n    input_set(', call_start)
(output / 'host_touch_actions_under_test.h').write_text(
    host[start:end] + '\nstatic void host_touch_sample(uint64_t physical_b, uint64_t scripted_b, uint64_t allowed, bool menu, bool typing) {\n'
    '    uint64_t b = physical_b | scripted_b, script_b = scripted_b;\n'
    '    int lx = 0, ly = 0, rx = 0, ry = 0;\n' + host[touch_start:touch_end] +
    '    b &= allowed;\n' + host[call_start:call_end] + '\n}\n', encoding='utf-8')

compiler = Path(os.environ.get('ANDROID_NDK_HOME', r'C:\Users\Public\Android\Sdk\ndk\30.0.16248370')) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
sdl = deps / 'SDL3-3.4.18'
common = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
          '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-DW_OK=2',
          '-Dstrtok_r=strtok_s', '-Wno-deprecated-declarations',
          '-I' + str(deps), '-I' + str(output), '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include')]
sources = [str(project / 'runtime' / name) for name in ('actions.c', 'config.c', 'config_store.c')]
shutil.copy2(sdl / 'lib/x64/SDL3.dll', output / 'SDL3.dll')
for name in ('test_touch_actions', 'test_actions'):
    exe = output / (name + '.exe')
    subprocess.run(common + [str(project / 'tests' / (name + '.c'))] + sources +
                   [str(sdl / 'lib/x64/SDL3.lib'), '-o', str(exe)], env=env, cwd=output, check=True, timeout=120)
    result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=60)
    (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    print(result.stdout.decode('utf-8', errors='replace'), end='')
    print(result.stderr.decode('utf-8', errors='replace'), end='')
    result.check_returncode()
print('Logs:', output)
