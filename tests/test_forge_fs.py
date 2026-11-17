"""Compile unchanged production Forge file adapters; no game, APK or device executes."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    depth = 0
    for end in range(source.index('{', start), len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1] + '\n'
    raise AssertionError('Unclosed production function: ' + signature)


def main():
    source_file = ROOT / 'runtime/forge.c'
    source = source_file.read_text(encoding='utf-8')
    fragment = function(source, 'static uint32_t gcall(')
    fragment += source[source.index('HLE_FUNC("forge_fs_openFile",'):source.index('// ------------------------------------------------------------------ nn::os wrappers')]
    assert fragment.count('HLE_FUNC(') == 4
    before = hashlib.sha256(source_file.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='mhgu-forge-fs-') as directory:
        work = Path(directory)
        (work / 'forge_fs_production.inc').write_text(fragment, encoding='utf-8')
        exe = work / ('test-forge-fs.exe' if os.name == 'nt' else 'test-forge-fs')
        compiler = shutil.which('cl')
        if compiler:
            command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX', '/utf-8',
                       '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(work), str(ROOT / 'tests/test_forge_fs.cpp'), '/Fe:' + str(exe)]
        else:
            compiler = shutil.which('c++') or shutil.which('clang++')
            assert compiler, 'A host C++17 compiler is required'
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(work),
                       str(ROOT / 'tests/test_forge_fs.cpp'), '-o', str(exe)]
        for step in [command, [str(exe)]]:
            result = subprocess.run(step, cwd=work, capture_output=True, timeout=60)
            print((result.stdout + result.stderr).decode('utf-8', errors='replace'), end='')
            result.check_returncode()
    assert hashlib.sha256(source_file.read_bytes()).hexdigest() == before, 'Production source changed during validation'


if __name__ == '__main__':
    main()
