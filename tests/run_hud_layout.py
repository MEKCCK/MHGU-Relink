"""Run host regressions against the actual production HUD layout helpers."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件')
output = workspace / 'analysis/ranged-r-hud-edit-0.2.2-r12-20261113/host-hud-layout'
output.mkdir(parents=True, exist_ok=True)
report_path = output / 'hud-layout-results.json'
sources = [
    'runtime/nvn/vulkan/hud_layout.h',
    'runtime/nvn/vulkan/exec.c',
    'tests/test_hud_layout.c',
    'tests/run_hud_layout.py',
]


def digests():
    return {name: hashlib.sha256((project / name).read_bytes()).hexdigest() for name in sources}


before = digests()
report = {
    'contract': 'production-hud-layout-r12',
    'passed': False,
    'source_frozen': False,
    'exit_code': 1,
    'source_root': str(project),
    'source_sha256_before': before,
    'logs': {},
    'method': 'Compile test_hud_layout.c including the actual production hud_layout.h; no copied helper formulas or game/Vulkan stubs.',
    'cases': [
        'Existing menu, reticle, loading-row and panel regressions at 16:9, 21:9, 32:9 and 16:10.',
        'Quest-only target, Hunter Arts and entire item/ammo group classification, including boundaries.',
        'Positive and negative offsets for every HUD region at four aspect ratios, hidden and invalid regions.',
        'Zero and single-axis offsets, effective-zero scales, manual feedback ownership and scissor relaxation.',
    ],
    'device_tested': False,
    'limits': [
        'CPU helper regression only; the game, renderer and Android device are not executed.',
        'Does not verify the rendered HUD, touch editor or device behavior.',
    ],
}

try:
    vcvars = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True,
                           text=True, timeout=60)
    setup_log = output / 'environment.log'
    setup_log.write_text(setup.stderr, encoding='utf-8')
    report['logs']['environment'] = str(setup_log)
    report['environment_exit_code'] = setup.returncode
    setup.check_returncode()
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key] = value
    compiler = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    exe = output / 'test-hud-layout.exe'
    command = [
        str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
        '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Werror',
        str(project / 'tests/test_hud_layout.c'), '-o', str(exe),
    ]
    report['compile_command'] = command
    build = subprocess.run(command, env=env, cwd=output, capture_output=True, timeout=120)
    build_log = output / 'build.log'
    build_log.write_bytes(build.stdout + build.stderr)
    report['logs']['build'] = str(build_log)
    report['build_exit_code'] = build.returncode
    if build.returncode:
        print((build.stdout + build.stderr).decode(errors='replace'), end='')
    build.check_returncode()

    run = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=30)
    regression_log = output / 'regression.log'
    regression_log.write_bytes(run.stdout + run.stderr)
    report['logs']['regression'] = str(regression_log)
    report['regression_exit_code'] = run.returncode
    print((run.stdout + run.stderr).decode(errors='replace'), end='')
    run.check_returncode()
    report['passed'] = True
    report['exit_code'] = 0
except Exception as error:
    report['error'] = str(error)
    print(f'FAIL: {error}', file=sys.stderr)
finally:
    after = digests()
    report['source_sha256_after'] = after
    report['source_frozen'] = before == after
    if not report['source_frozen']:
        report['passed'] = False
        report['exit_code'] = 1
        report['error'] = 'Source changed during the host HUD layout regression'
        print('FAIL:', report['error'], file=sys.stderr)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Report:', report_path)

sys.exit(report['exit_code'])
