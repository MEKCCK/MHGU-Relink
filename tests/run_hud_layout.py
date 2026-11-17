"""Run host regressions against the actual production HUD layout helpers."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(Path(__file__).resolve().parents[1])))
output = workspace / 'analysis/hud-panel-status-0.2.4-20261117/host-hud-layout'
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
    'contract': 'production-hud-layout-quest-panel-status',
    'passed': False,
    'source_frozen': False,
    'exit_code': 1,
    'source_root': str(project),
    'source_sha256_before': before,
    'logs': {},
    'method': 'Compile the actual production hud_layout.h plus verbatim quest gate, grouping decision and CPU group functions from exec.c; only quest presence and renderer globals/logging are fixtures.',
    'cases': [
        'Existing menu, reticle, loading-row and panel regressions at 16:9, 2.2, 21:9, 32:9 and 16:10.',
        'Actual grouping eligibility and final group functions preserve independently layered menu body/text/right-border X in 21:9, 32:9 and Edges HUD placement, with 16:9 control, across all five render aspects; non-menu grouping and custom quest Arts offsets remain active.',
        'Quest-only target, Hunter Arts and entire item/ammo group classification, including boundaries.',
        'All 256 combinations of actual quest presence and seven production HUD exclusions, using the extracted production gate.',
        'Synthetic village-panel overlap points and top status-bar endpoints receive no quest HUD offsets; actual quest map, item/ammo, Hunter Arts and target offsets remain active.',
        'Positive and negative offsets for every HUD region at five aspect ratios, hidden and invalid regions.',
        'Zero and single-axis offsets, effective-zero scales, manual feedback ownership and scissor relaxation.',
    ],
    'device_tested': False,
    'limits': [
        'CPU helpers, gate and grouping use synthetic matrices; full renderer command dispatch, GPU draws, the game and Android device are not executed.',
        'presence_in_quest is a controlled stub; actual quest-state detection is not executed.',
        'Does not verify the rendered HUD, touch editor or device behavior.',
    ],
}

try:
    renderer = (project / 'runtime/nvn/vulkan/exec.c').read_text(encoding='utf-8')
    gates = re.findall(r'^[ \t]*bool hunt_hud\s*=[^;]+;', renderer, re.MULTILINE)
    if len(gates) != 1:
        raise RuntimeError(f'Expected one production hunt_hud declaration, found {len(gates)}')
    gate = gates[0].strip()
    report['production_gate'] = gate
    report['production_gate_sha256'] = hashlib.sha256(gate.encode()).hexdigest()
    (output / 'hud_hunt_gate_under_test.h').write_text(
        'static bool hud_test_in_quest;\n'
        'int presence_in_quest(void) { return hud_test_in_quest; }\n'
        'static bool hud_test_hunt_gate(bool quest, bool ui_menu_pass, bool ui_results_pass,\n'
        '    bool ui_results_last, bool ui_loading_pass, bool loading_candidate,\n'
        '    bool ui_title_pass, bool ui_meownster_pass) {\n'
        '    hud_test_in_quest = quest;\n'
        f'    {gate}\n'
        '    return hunt_hud;\n'
        '}\n', encoding='utf-8')
    group_start = '#define UI_GROUP_MAX '
    group_end = 'static VkRenderPass pass_rp;'
    if renderer.count(group_start) != 1 or renderer.count(group_end) != 1:
        raise RuntimeError('Expected unique production CPU group block boundaries')
    group_cpu = renderer[renderer.index(group_start):renderer.index(group_end)]
    recent = re.findall(r'^#define UI_RECENT [^\n]+\nstatic struct [^\n]+ ui_recent\[UI_RECENT\];', renderer, re.MULTILINE)
    decisions = re.findall(r'^[ \t]*grouped = sy < 1\.0f[\s\S]*?^[ \t]*grouped = hud_touch_group_allowed\(grouped, offset\);', renderer, re.MULTILINE)
    if len(recent) != 1 or len(decisions) != 1:
        raise RuntimeError('Expected unique production recent declaration and grouping decision')
    decision = decisions[0].strip()
    report['production_group_cpu_sha256'] = hashlib.sha256(group_cpu.encode()).hexdigest()
    report['production_group_decision'] = decision
    report['production_group_decision_sha256'] = hashlib.sha256(decision.encode()).hexdigest()
    (output / 'hud_group_under_test.h').write_text(
        'static float g_render_aspect, g_ui_anchor_aspect;\n'
        'static bool ui_loading_pass, ui_frontend_pass, ui_title_pass, ui_stamp_pass;\n'
        'static bool ui_results_pass, ui_results_last, ui_meownster_pass;\n'
        'static uint32_t ui_nrecent;\n'
        'static int g_hudtrace_active;\n'
        '#define LOG(...) ((void)0)\n'
        + recent[0] + '\n' + group_cpu + '\n'
        'static bool hud_test_grouped(float s, float sy, float pos_z, float pos_y,\n'
        '    bool menu_canvas, bool arts_move, bool target_move, HudTouchOffset offset) {\n'
        '    bool grouped;\n'
        '    (void)menu_canvas;\n'
        f'    {decision}\n'
        '    return grouped;\n'
        '}\n', encoding='utf-8')
    vcvars = os.environ.get('VCVARS64', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
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
    compiler = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973'))) / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    exe = output / 'test-hud-layout.exe'
    command = [
        str(compiler), '--target=x86_64-pc-windows-msvc', '-fuse-ld=link', '-std=gnu11',
        '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Werror',
        '-I' + str(output),
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
