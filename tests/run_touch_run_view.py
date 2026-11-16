"""Verify native stick-run routing and the real input publication snapshot.

The C test includes production input_view.c and an exact source slice of input.c.
Original ARM32 MB1/MB2 translators provide the fixture maps; no game is launched.
"""
from pathlib import Path
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(project)))
output = Path(os.environ.get('MHGU_TEST_OUTPUT', str(workspace / 'analysis/relink-touch-run-view')))
deps = workspace / 'analysis/host-test-deps'
output.mkdir(parents=True, exist_ok=True)
report_path = output / 'touch-run-view-results.json'
sources = ['runtime/hle/input.c', 'runtime/hle/input.h', 'runtime/hle/input_view.c',
           'runtime/hle/input_view.h', 'runtime/actions.c', 'runtime/actions.h',
           'runtime/config.c', 'runtime/config.h', 'runtime/config_store.c',
           'runtime/host_input.c', 'runtime/touch.cpp',
           'runtime/touch.h', 'tests/test_touch_run_view.c', 'tests/run_touch_run_view.py']


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshots():
    return {name: digest(project / name) for name in sources}


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f'Set {name} to the required local path before running this host check')
    return Path(value)


def native_translator_fixture():
    sys.path.insert(0, str(project / 'tools'))
    if os.environ.get('MHGU_UNICORN_PATH'):
        sys.path.insert(0, os.environ['MHGU_UNICORN_PATH'])
    original = required_path('MHGU_NATIVE_MAIN')
    from nso import Nso
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
    from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP,
                                   UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR)
    expected_sha = 'b3935415499549adaf4c0e89822831daa175cfd456a7990791c79be3997645ef'
    assert digest(original) == expected_sha, 'Unexpected original game NSO'
    nso = Nso(str(original))
    assert nso.build_id == 'fb08f1d20fd1204fe7070672777f869b', 'Unexpected original build ID'
    base, returned = 0x200000, 0x0f000000
    image = bytearray(nso.image)
    for offset, kind, _symbol, addend in nso.relocs:
        if kind == 23:
            struct.pack_into('<I', image, offset, (base + addend) & 0xffffffff)
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(base, nso.size)
    uc.mem_write(base, bytes(image))
    uc.mem_map(0x08000000, 0x10000)
    uc.mem_map(returned, 0x1000)

    def translate(entry, word, argument=0x08000000):
        uc.reg_write(UC_ARM_REG_CPSR, 0x10)
        uc.reg_write(UC_ARM_REG_R0, argument)
        uc.reg_write(UC_ARM_REG_R1, word)
        uc.reg_write(UC_ARM_REG_SP, 0x0800f000)
        uc.reg_write(UC_ARM_REG_LR, returned)
        uc.emu_start(entry, returned, count=256)
        assert uc.reg_read(UC_ARM_REG_PC) == returned, 'Translator did not return in its instruction budget'
        return uc.reg_read(UC_ARM_REG_R0)

    generated, evidence = [], []
    rng = random.Random(0x5de4b0)
    words = [0, 0xffffffff, 0x200, 0x800, 0xa00, 0xf0000]
    words += [(1 << n) | (1 << (n + 1)) for n in range(31)]
    words += [rng.getrandbits(32) for _ in range(512)]
    for name, entry in [('mb1', 0x5de4b0), ('mb2', 0x5de4f8)]:
        assert translate(entry, 0) == 0, 'Translator is not a zero-based bit mapping'
        bits = [translate(entry, 1 << n) for n in range(32)]
        for word in words:
            mapped = 0
            for n, value in enumerate(bits):
                if word & (1 << n):
                    mapped |= value
            assert mapped == translate(entry, word), ('Nonlinear translator input', name, word)
        generated.append('static const uint32_t native_' + name + '_bits[32] = {\n    ' +
                         ', '.join(f'0x{value:08x}u' for value in bits) + '\n};\n')
        evidence.append(dict(name=name, address=hex(entry), basis=bits, combination_checks=len(words)))
    fixture = output / 'native_translator_fixture.h'
    fixture.write_text(''.join(generated), encoding='utf-8')
    report['native_translators'] = dict(main=str(original), sha256=expected_sha,
        build_id=nso.build_id, fixtures=evidence, generated_header_sha256=digest(fixture))
    setter_checks = 0
    for event in (0, 31):
        for initial in (0, 0x40, 0x800, 0x12345678, 0x80000001):
            player = 0x08002000
            before = bytearray([0x5a] * 0x4000)
            offset = 0x1414 + 4 * (event >> 5)
            struct.pack_into('<I', before, offset, initial)
            expected = bytearray(before)
            struct.pack_into('<I', expected, offset, initial | (1 << (event & 31)))
            uc.mem_write(player, bytes(before))
            translate(0x485de4, event, player)
            assert bytes(uc.mem_read(player, len(before))) == bytes(expected), 'Native event setter fixture mismatch'
            setter_checks += 1
    report['native_event_setter'] = dict(address='0x485de4', checks=setter_checks,
        semantics='word[pl+0x1414+4*(event>>5)] |= 1<<(event&31)', events=[0, 31])


report = dict(passed=False, exit_code=1, source_root=str(project), logs={},
    method='Production input_view.c, real actions implementation, exact input.c publication slice and original ARM32 translator fixtures',
    limits=['Mutex interleavings are deterministic boundary schedules, not a threaded stress test.',
            'Original translators execute only to obtain pure input mappings; native movement, animation and rendering are not executed.',
            'No game, save or Android device is used.'])
before = snapshots()
report['source_sha256'] = before
try:
    text = (project / 'runtime/hle/input.c').read_text(encoding='utf-8')
    start = text.index('static volatile uint64_t g_buttons;')
    end = text.index('HLE_FUNC(', start)
    section = text[start:end]
    assert section.count('touch_run') >= 4, 'Input publication must copy touch_run at both boundaries'
    extracted = output / 'input_publication_under_test.h'
    extracted.write_text(section, encoding='utf-8')
    assert extracted.read_text(encoding='utf-8') == section
    report['input_source_extraction'] = dict(start_character=start, end_character=end,
        sha256=digest(extracted), exact_source_match=True)
    native_translator_fixture()
    vcvars = required_path('VCVARS64')
    setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True,
                           text=True, check=True, timeout=60)
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key] = value
    ndk = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973')))
    compiler = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    sdl = deps / 'SDL3-3.4.18'
    shutil.copy2(sdl / 'lib/x64/SDL3.dll', output / 'SDL3.dll')
    exe = output / 'test-touch-run-view.exe'
    command = [str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
               '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-DW_OK=2',
               '-Dstrtok_r=strtok_s', '-Wno-deprecated-declarations', '-Wall', '-Wextra',
               '-I' + str(project / 'tests/host_compat'), '-I' + str(deps), '-I' + str(output),
               '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include'),
               str(project / 'tests/test_touch_run_view.c')]
    command += [str(project / 'runtime' / name) for name in ('actions.c', 'config.c', 'config_store.c')]
    command += [str(sdl / 'lib/x64/SDL3.lib'), '-o', str(exe)]
    result = subprocess.run(command, env=env, cwd=output, capture_output=True, timeout=120)
    log = output / 'touch-run-view-build.log'
    log.write_bytes(result.stdout + result.stderr)
    report['logs']['build'] = str(log)
    if result.returncode:
        print(log.read_text(encoding='utf-8', errors='replace'))
    result.check_returncode()
    result = subprocess.run([str(exe)], env=env, cwd=output, capture_output=True, timeout=60)
    log = output / 'touch-run-view.log'
    log.write_bytes(result.stdout + result.stderr)
    report['logs']['regression'] = str(log)
    print(result.stdout.decode('utf-8', errors='replace'), end='')
    print(result.stderr.decode('utf-8', errors='replace'), end='')
    result.check_returncode()
    assert b'PASS_GROUPS=7' in result.stdout, 'All seven run-view regression groups must execute'
    report['passed_groups'] = 7
    after = snapshots()
    report['source_sha256_after'] = after
    report['source_unchanged'] = before == after
    assert report['source_unchanged'], 'Source changed while this regression ran'
    assert extracted.read_text(encoding='utf-8') == section
    report['passed'], report['exit_code'] = True, 0
except Exception as error:
    report['error'] = f'{type(error).__name__}: {error}'
    print('FAIL:', report['error'], file=sys.stderr)
finally:
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Report:', report_path)
sys.exit(report['exit_code'])
