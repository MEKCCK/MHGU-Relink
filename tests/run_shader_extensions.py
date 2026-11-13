"""Compile translator cases, and optionally reproduce shaders from a user's text log."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--log', type=Path)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
project = Path(__file__).resolve().parents[1]
output = args.output or Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件/analysis/startup-0.2.1-20261113/host-shaders')
output.mkdir(parents=True, exist_ok=True)
llvm = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/30.0.16248370'
glslc = llvm / 'shader-tools/windows-x86_64/glslc.exe'
vcvars = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat')
setup = subprocess.run('call "' + str(vcvars) + '" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
exe = output / 'test-shader-extensions.exe'
command = [str(llvm / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'),
           '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11', '-O2', '-D_CRT_SECURE_NO_WARNINGS',
           str(project / 'tests/test_shader_extensions.c'), '-o', str(exe)]
result = subprocess.run(command, env=env, capture_output=True, timeout=60)
(output / 'build.log').write_bytes(result.stdout + result.stderr)
if result.returncode:
    print((result.stdout + result.stderr).decode(errors='replace'))
result.check_returncode()
subprocess.run([str(exe), str(output)], env=env, check=True)
extension = '#extension GL_EXT_texture_shadow_lod : require\n'
samples = sorted(p for p in output.iterdir() if p.suffix in ('.vert', '.frag', '.comp') and not p.name.startswith('logged-'))
records = []
for source in samples:
    result = subprocess.run([str(glslc), '--target-env=vulkan1.2', str(source), '-o', str(source) + '.spv'], capture_output=True)
    (output / (source.name + '.log')).write_bytes(result.stdout + result.stderr)
    if result.returncode and source.name.startswith('required-'):
        assert b'extension not supported: GL_EXT_texture_shadow_lod' in result.stderr, result.stderr.decode()
    else:
        assert result.returncode == 0, result.stderr.decode()
    records.append({'source': source.name, 'compiled': result.returncode == 0, 'extension_required': source.name.startswith('required-')})

if args.log:
    lines = args.log.read_text(encoding='utf-8-sig').splitlines(keepends=True)
    sources = {}
    for start, line in enumerate(lines):
        if not line.startswith('#version '):
            continue
        inside = False
        depth = 0
        for end in range(start, len(lines)):
            if lines[end].startswith('void main('):
                inside = True
            if inside:
                depth += lines[end].count('{') - lines[end].count('}')
                if depth == 0:
                    source = ''.join(lines[start:end+1])
                    sources[hashlib.sha256(source.encode()).hexdigest()] = source
                    break
    assert sources, 'No complete logged GLSL samples'
    for index, source in enumerate(sources.values()):
        assert 'Shadow' not in source, 'A logged shader needs a separate shadow sampling check'
        old = output / ('logged-%02d-old.vert' % index)
        new = output / ('logged-%02d-fixed.vert' % index)
        old.write_text(source, encoding='utf-8')
        new.write_text(source.replace(extension, ''), encoding='utf-8')
        for target in ('vulkan1.1', 'vulkan1.2'):
            results = []
            for path in (old, new):
                result = subprocess.run([str(glslc), '--target-env=' + target, str(path), '-o', str(path) + '.spv'], capture_output=True)
                (output / (path.name + '-' + target + '.log')).write_bytes(result.stdout + result.stderr)
                results.append(result)
            assert results[0].returncode != 0 and b'extension not supported: GL_EXT_texture_shadow_lod' in results[0].stderr
            assert results[1].returncode == 0, results[1].stderr.decode()
            records.append({'logged_sample': index, 'target': target, 'before_failed': True, 'after_compiled': True})
    print('PASS:', len(sources), 'unique logged failures reproduce; corrected shaders compile for Vulkan 1.1 and 1.2.')
(output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('PASS: shader extension generation and compiler regression checks.')
