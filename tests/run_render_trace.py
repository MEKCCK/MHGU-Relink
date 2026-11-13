"""Exercise the verbatim production CPU render-trace state machine on the host."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
output = workspace / 'analysis/controller-ip-0.2.2-20261113/host-render-trace'
output.mkdir(parents=True, exist_ok=True)
source = (project / 'runtime/nvn/vulkan/exec.c').read_text(encoding='utf-8')
start = source.index('// BEGIN CPU RENDER TRACE')
end = source.index('// END CPU RENDER TRACE', start)
code = source[start:end]
# Scan trace_draw too: its metadata walk is outside the host-compiled state-machine block.
trace_functions = dict(re.findall(
    r'^(?:static )?(?:void|bool) ((?:vk_)?trace_[a-z_]+)\([^\n]*\) (\{(?:[^\n]*\}|[\s\S]*?^\}))',
    source, re.MULTILINE))
assert 'trace_draw' in trace_functions and 'vk_trace_frame' in trace_functions
gpu_calls = r'\b(?:vk[A-Z]\w*|vk_(?:wait|readback|copy)\w*)\s*\('
for name, body in trace_functions.items():
    assert not re.search(gpu_calls, body), f'{name} must remain CPU-only'
# The state machine also links no Vulkan stubs, so accidental GPU calls fail to link.
assert not re.search(gpu_calls, code)
(output / 'render_trace_under_test.h').write_text(code, encoding='utf-8')

vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/30.0.16248370'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
exe = output / 'render-trace.exe'
build = subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
    '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(output), str(project / 'tests/test_render_trace.c'), '-o', str(exe)],
    env=env, capture_output=True, timeout=120)
(output / 'build.log').write_bytes(build.stdout + build.stderr)
if build.returncode:
    print((build.stdout + build.stderr).decode(errors='replace'))
build.check_returncode()
run = subprocess.run([str(exe)], env=env, capture_output=True, timeout=30)
(output / 'regression.log').write_bytes(run.stdout + run.stderr)
print((run.stdout + run.stderr).decode(errors='replace'), end='')
run.check_returncode()
(output / 'results.json').write_text(json.dumps({
    'test': 'production-cpu-render-trace', 'passed': True,
    'production_block_sha256': hashlib.sha256(code.encode()).hexdigest(),
    'gpu_scan_functions': sorted(trace_functions),
    'gpu_scan_sha256': hashlib.sha256(''.join(trace_functions.values()).encode()).hexdigest(),
    'gpu_calls_linked': False, 'device_tested': False,
}, indent=2), encoding='utf-8')
