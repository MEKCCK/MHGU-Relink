"""Verify bundled Forge provenance plus actual Java copy/native lookup behavior on the host.

The parser/copy/lookup functions are extracted unchanged; Android storage and
AssetManager are fixtures. This does not execute the APK, console NSO or game.
Optionally pass the supplied Forge ZIP to compare every original archive member.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'android/app/src/main/assets'


def function(source, signature):
    start = source.index(signature)
    depth = 0
    for end in range(source.index('{', start), len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1] + '\n'
    raise AssertionError('Unclosed production function: ' + signature)


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=120, shell=isinstance(command, str))
    print((result.stdout + result.stderr).decode('utf-8', errors='replace'), end='')
    result.check_returncode()


def main():
    manifest = json.loads((ROOT / 'android/forge-v2.0.0.json').read_text(encoding='utf-8'))
    assert manifest['version'] == '2.0.0'
    resources = manifest['resources']
    expected = {item['asset'] for item in resources}
    assert len(expected) == 5
    assert {p.relative_to(ASSETS).as_posix() for p in (ASSETS / 'forge-v2.0.0').rglob('*') if p.is_file()} == expected
    # Keep the older resources as requested; verify their provenance too.
    legacy = json.loads((ROOT / 'android/forge-v1.5.1.json').read_text(encoding='utf-8'))
    for item in legacy['resources']:
        data = (ASSETS / item['asset']).read_bytes()
        assert len(data) == item['size'] and hashlib.sha256(data).hexdigest() == item['sha256']
    for item in resources:
        data = (ASSETS / item['asset']).read_bytes()
        assert len(data) == item['size'] and hashlib.sha256(data).hexdigest() == item['sha256']
    if len(sys.argv) > 1:
        archive = Path(sys.argv[1])
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == manifest['archive_sha256']
        with zipfile.ZipFile(archive) as z:
            entries = resources + manifest['excluded']
            assert {i.filename for i in z.infolist() if not i.is_dir()} == {i['member'] for i in entries}
            assert not any(i.filename.lower().endswith('.nro') for i in z.infolist())
            for item in entries:
                data = z.read(item['member'])
                assert len(data) == item['size'] and hashlib.sha256(data).hexdigest() == item['sha256']
    print('PASS: active Forge 2.0.0 and retained legacy resources match their provenance manifests')

    java = (ROOT / 'android/app/src/main/java/com/mhgu/relink/GameActivity.java').read_text(encoding='utf-8')
    assert 'prepareForgeAssets();' in function(java, 'private void prepareFiles(')
    assert java.index('prepareFiles();') < java.index('super.onCreate(')
    native = (ROOT / 'runtime/forge.c').read_text(encoding='utf-8')
    fragment = native[native.index('// ---------------------------------------------------------------- config.ini'):native.index('// The trampoline and stub regions')]
    with tempfile.TemporaryDirectory(prefix='mhgu-forge-builtin-') as directory:
        work = Path(directory)
        java_test = (ROOT / 'tests/java/ForgeBuiltinTest.java').read_text(encoding='utf-8')
        methods = function(java, 'private void prepareForgeAssets(') + function(java, 'private boolean copyAsset(')
        (work / 'ForgeBuiltinTest.java').write_text(java_test.replace('// PRODUCTION_METHODS', methods), encoding='utf-8')
        javac = shutil.which('javac')
        java_exe = shutil.which('java')
        assert javac and java_exe, 'JDK 17 is required'
        run([javac, '-encoding', 'UTF-8', str(work / 'ForgeBuiltinTest.java')], work)
        run([java_exe, '-ea', '-cp', str(work), 'ForgeBuiltinTest', str(work / 'java-internal'), str(ASSETS)], work)
        (work / 'forge_builtin.inc').write_text(fragment, encoding='utf-8')
        vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
        assert install, 'MSVC C++ build tools are required'
        vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
        exe = work / 'test-forge-builtin.exe'
        compile_args = ['cl', '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(work),
                        str(ROOT / 'tests/test_forge_builtin.cpp'), '/Fe:' + str(exe)]
        run('call ' + subprocess.list2cmdline([str(vcvars)]) + ' >nul && ' + subprocess.list2cmdline(compile_args), work)
        run([str(exe), str(work / 'native-internal'), str(ASSETS)], work)


if __name__ == '__main__':
    main()
