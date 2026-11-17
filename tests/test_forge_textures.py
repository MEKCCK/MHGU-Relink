"""Exercise production Forge texture helpers without a GPU, game or device."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    configured = os.environ.get('MHGU_FORGE_TEXTURES_TEST_OUTPUT')
    out = Path(configured) if configured else Path(tempfile.mkdtemp(prefix='mhgu-forge-textures-'))
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'tests/test_forge_textures.cpp'
    inputs = [source, ROOT / 'runtime/forge_textures.h', ROOT / 'runtime/forge_texture_command.h']
    hashes = lambda: {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    before = hashes()
    exe = out / ('test-forge-textures.exe' if os.name == 'nt' else 'test-forge-textures')
    imgui = ROOT / 'third_party/cimgui/imgui'
    compiler = shutil.which('cl')
    if compiler or os.name == 'nt':
        command = [compiler or 'cl', '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX',
                   '/I' + str(imgui), str(source), '/Fe:' + str(exe)]
        if not compiler:
            vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
            install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
            assert install, 'MSVC C++ build tools are required'
            vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
            command = 'call ' + subprocess.list2cmdline([str(vcvars)]) + ' >nul && ' + subprocess.list2cmdline(command)
    else:
        compiler = shutil.which('c++') or shutil.which('clang++')
        assert compiler, 'A host C++17 compiler is required'
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(imgui), str(source), '-o', str(exe)]
    build = subprocess.run(command, cwd=out, shell=isinstance(command, str), capture_output=True, timeout=60)
    (out / 'build.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        print((build.stdout + build.stderr).decode('utf-8', errors='replace'))
    build.check_returncode()
    run = subprocess.run([str(exe)], cwd=out, capture_output=True, timeout=30)
    (out / 'regression.log').write_bytes(run.stdout + run.stderr)
    print((run.stdout + run.stderr).decode('utf-8', errors='replace'), end='')
    assert hashes() == before, 'Test inputs changed during validation'
    (out / 'result.json').write_text(json.dumps(dict(passed=run.returncode == 0, exit_code=run.returncode,
        source_sha256=before, source_unchanged=True, device_tested=False), indent=2), encoding='utf-8')
    print('Evidence:', out)
    run.check_returncode()


if __name__ == '__main__':
    main()
