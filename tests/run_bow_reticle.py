"""Run the native Bow world-guide gate contract against the actual production module."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
output = workspace / 'analysis' / 'bow-charge-drag-0.2.2-r14-20261113'
output.mkdir(parents=True, exist_ok=True)
deps = workspace / 'analysis' / 'host-test-deps'
report_path = output / 'guide-regression-results.json'
frozen_inputs = {
    'runtime/hle/weapon.c': '39cc465e98ab1ab52ee5edae8b09e6489a1fd76fea09906b30c2dad3fd9b6f98',
}
sources = [*frozen_inputs, 'runtime/hle/mousecam.c', 'tests/test_ranged_input.c',
           'runtime/hle/bow_reticle.c', 'tests/test_bow_reticle.c', 'tests/run_bow_reticle.py']


def digests():
    return {name: hashlib.sha256((project / name).read_bytes()).hexdigest() for name in sources}


before = digests()
report = {
    'contract': 'bow-charge-guide-v1',
    'passed': False,
    'baseline_failed_expected': False,
    'source_frozen': False,
    'exit_code': 1,
    'source_root': str(project),
    'source_sha256': before,
    'logs': {},
    'method': 'Host regression of production world-guide visibility hook and weapon resolver with synthetic guest memory; native charge predicate fixture from local static guest evidence',
    'native_main_build_id': 'fb08f1d20fd1204fe7070672777f869b',
    'native_main_sha256': 'b3935415499549adaf4c0e89822831daa175cfd456a7990791c79be3997645ef',
    'limits': [
        'Tests the exact effect-4 gate; downstream native motion, geometry and rendering are not executed.',
        'No target game, save, Android device or visual run was executed by this regression.',
        'The pure native charging query is simulated from the independently decoded action set in this host test.',
    ],
}

try:
    for name, expected in frozen_inputs.items():
        if before[name] != expected:
            raise RuntimeError(f'Prior input source changed: {name}')

    vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True,
                           text=True, check=True, timeout=60)
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key] = value
    compiler = Path(os.environ.get('ANDROID_NDK_HOME', r'C:\Users\Public\Android\Sdk\ndk\30.0.16248370')) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    exe = output / 'test-bow-reticle.exe'
    build = subprocess.run([
        str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
        '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE',
        '-Wno-deprecated-declarations', '-Wall', '-Wextra', '-Werror',
        '-I' + str(deps), '-I' + str(project / 'runtime'),
        str(project / 'tests' / 'test_bow_reticle.c'), '-o', str(exe),
    ], env=env, cwd=output, capture_output=True, timeout=120)
    build_log = output / 'reticle-build.log'
    build_log.write_bytes(build.stdout + build.stderr)
    report['logs']['build'] = str(build_log)
    build.check_returncode()

    baseline = subprocess.run([str(exe), '--baseline'], env=env, cwd=output,
                              capture_output=True, timeout=30)
    baseline_log = output / 'reticle-baseline.log'
    baseline_log.write_bytes(baseline.stdout + baseline.stderr)
    report['logs']['baseline'] = str(baseline_log)
    report['baseline_exit_code'] = baseline.returncode
    report['baseline_failed_expected'] = baseline.returncode == 1 and b'EXPECTED_BASELINE_CONTRACT_FAILURES=1' in baseline.stdout
    if not report['baseline_failed_expected']:
        raise RuntimeError('Unhooked baseline did not fail the expected charge-guide case')

    fixed = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=30)
    fixed_log = output / 'reticle-fixed.log'
    fixed_log.write_bytes(fixed.stdout + fixed.stderr)
    report['logs']['fixed'] = str(fixed_log)
    report['fixed_exit_code'] = fixed.returncode
    print(fixed.stdout.decode('utf-8', errors='replace'), end='')
    print(fixed.stderr.decode('utf-8', errors='replace'), end='')
    fixed.check_returncode()
    after = digests()
    report['source_sha256_after'] = after
    report['source_frozen'] = before == after
    if not report['source_frozen']:
        raise RuntimeError('Source changed during baseline/fixed regression')
    report['passed'] = True
    report['exit_code'] = 0
except Exception as error:
    report['error'] = str(error)
    print(f'FAIL: {error}', file=sys.stderr)
finally:
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Report:', report_path)

sys.exit(report['exit_code'])
