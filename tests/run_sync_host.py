"""Force synchronization/lifetime schedules against verbatim production code."""
from pathlib import Path
import json
import os
import subprocess

project = Path(__file__).resolve().parents[1]
output = Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件/analysis/relink-0.2.2-20261113/host-sync')
output.mkdir(parents=True, exist_ok=True)
vcvars = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
compiler = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'

def section(source, start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]

def function(source, name):
    import re
    match = re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start()
    depth = 0
    for pos in range(source.index('{', start), len(source)):
        depth += (source[pos] == '{') - (source[pos] == '}')
        if depth == 0:
            return source[start:pos+1] + '\n'
    raise AssertionError(name)

def compile_run(name, test, reference):
    exe = output / (name + '.exe')
    command = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
               '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-Wno-deprecated-declarations',
               '-DREFERENCE_BUILD=' + str(int(reference)), '-I' + str(project / 'tests/sync_host'),
               '-I' + str(output), '-I' + str(output.parents[1] / 'host-test-deps'),
               '-I' + str(project / 'runtime'), str(project / 'tests' / test), '-o', str(exe)]
    build = subprocess.run(command, env=env, capture_output=True, timeout=120)
    (output / (name + '-build.log')).write_bytes(build.stdout + build.stderr)
    if build.returncode:
        print((build.stdout + build.stderr).decode(errors='replace'))
    build.check_returncode()
    result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=60)
    (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    print((result.stdout + result.stderr).decode(errors='replace'), end='')
    result.check_returncode()
    return {'test': name, 'passed': True, 'reference': reference}

records = []
for reference in (True, False):
    source = subprocess.check_output(['git', 'show', '35a8c23:runtime/kernel.c'], cwd=project).decode() if reference else (project / 'runtime/kernel.c').read_text(encoding='utf-8')
    code = section(source, '#define R_OK_', '// ------------------------------------------------------------------ handle table')
    code += 'volatile int g_rt_unwind;\n' + function(source, 'wake')
    code += section(source, 'static void deadline(', '// ------------------------------------------------------------------ memory map')
    code += section(source, 'static uint32_t *u32p(', '// ------------------------------------------------------------------ address arbiter')
    (output / 'kernel_under_test.h').write_text(code, encoding='utf-8')
    records.append(compile_run('kernel-' + ('baseline' if reference else 'fixed'), 'test_kernel_sync.c', reference))

for reference in (True, False):
    source = subprocess.check_output(['git', 'show', '35a8c23:runtime/hle/movie.c'], cwd=project).decode() if reference else (project / 'runtime/hle/movie.c').read_text(encoding='utf-8')
    code = section(source, '#define MOVIE_EOS', '// Strings handed to the game')
    code += function(source, 'self_state')
    code += section(source, '#define NIN', '// Input / output buffers')
    code += 'static bool flush_pending(Decoder *d) { (void)d; return false; }\n'
    for name in ('dec_signal', 'dec_start', 'dec_stop', 'dec_release_output', 'dec_dtor'):
        code += function(source, name)
    if not reference:
        code += function(source, 'dec_destroy')
        code += section(source, '#define DEC_RETAINED(fn)', 'DEC_RETAINED(dec_configure)')
        code += 'DEC_RETAINED(dec_release_output)\n'
    (output / 'movie_under_test.h').write_text(code, encoding='utf-8')
    records.append(compile_run('movie-' + ('baseline' if reference else 'fixed'), 'test_movie_lifetime.c', reference))

source = (project / 'runtime/hle/presence.c').read_text(encoding='utf-8')
code = section(source, '#define G_GAMECONTROL', 'static const char *const g_weapons')
code += function(source, 'sane')
code += section(source, 'void presence_tick(void)', '    int party = 0, party_max = 0;') + '}\n'
(output / 'presence_under_test.h').write_text(code, encoding='utf-8')
records.append(compile_run('presence-fixed', 'test_presence_scene.c', False))
(output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('PASS: deterministic production synchronization regression.')
