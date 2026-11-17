"""Run production Forge plugin initialization/version gates without executing a game or APK."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import tempfile

from test_forge_builtin import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT / 'runtime/forge.c'
    header = ROOT / 'runtime/forge.h'
    fixture = ROOT / 'tests/test_forge_plugin_init.cpp'
    inputs = [source, header, fixture, Path(__file__).resolve(), ROOT / 'tests/test_forge_builtin.py']
    hashes = lambda: {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    before = hashes()
    native = source.read_text(encoding='utf-8')
    definitions = native + '\n' + header.read_text(encoding='utf-8')
    macros = []
    for name in ('FORGE_MAJOR', 'FORGE_MINOR', 'FORGE_PATCH', 'OFF_APP_RUN'):
        matches = re.findall(r'^#define\s+' + name + r'\s+[^\r\n]+', definitions, re.MULTILINE)
        assert len(matches) == 1, 'Expected one production definition: ' + name
        macros.append(matches[0])
    start = native.index('typedef struct {', native.index('#define MAX_PLUGINS'))
    end = native.index('} Plugin;', start) + len('} Plugin;')
    types = '\n'.join(macros) + '\n' + native[start:end] + '\nstatic uint32_t nop_fn;\n'
    runtime = ''.join(function(native, signature) for signature in ('static uint32_t text_addr(', 'static uint32_t gcall('))
    lifecycle = native.index('static uint32_t nvn_device, nvn_queue;')
    runtime += native[lifecycle:native.index('void forge_plugins_imgui(', lifecycle)]
    runtime += ''.join(function(native, signature) for signature in (
        'static void plugin_initializers(', 'int forge_plugin_titles(', 'static void app_run('))
    configured = os.environ.get('MHGU_FORGE_PLUGIN_TEST_OUTPUT')
    out = Path(configured) if configured else Path(tempfile.mkdtemp(prefix='mhgu-forge-plugin-init-'))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'result.json').unlink(missing_ok=True)
    (out / 'forge_plugin_types.inc').write_text(types, encoding='utf-8')
    (out / 'forge_plugin_runtime.inc').write_text(runtime, encoding='utf-8')
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    assert install, 'MSVC C++ build tools are required'
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    executable = out / 'test-forge-plugin-init.exe'
    arguments = ['cl', '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/O1', '/D_CRT_SECURE_NO_WARNINGS',
                 '/I' + str(out), str(fixture), '/Fe:' + str(executable)]
    command = 'call ' + subprocess.list2cmdline([str(vcvars)]) + ' >nul && ' + subprocess.list2cmdline(arguments)
    build = subprocess.run(command, shell=True, cwd=out, capture_output=True, timeout=120)
    (out / 'build.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        print((build.stdout + build.stderr).decode('utf-8', errors='replace'), end='')
    build.check_returncode()
    result = subprocess.run([str(executable)], cwd=out, capture_output=True, timeout=30)
    log = result.stdout + result.stderr
    (out / 'regression.log').write_bytes(log)
    print(log.decode('utf-8', errors='replace'), end='')
    unchanged = hashes() == before
    (out / 'result.json').write_text(json.dumps({'passed': result.returncode == 0 and unchanged,
        'exit_code': result.returncode, 'source_sha256': before, 'source_unchanged': unchanged,
        'production_functions': ['gcall', 'plugin_initializers', 'forge_plugin_titles', 'app_run',
            'forge_nvn_device', 'forge_plugins_graphics_ready', 'plugins_nvn',
            'forge_plugins_nvn_init', 'forge_plugins_nvn_ready', 'forge_nvn_present'],
        'device_tested': False}, indent=2) + '\n', encoding='utf-8')
    print('Evidence:', out)
    assert unchanged, 'Production/test inputs changed during validation'
    result.check_returncode()


if __name__ == '__main__':
    main()
