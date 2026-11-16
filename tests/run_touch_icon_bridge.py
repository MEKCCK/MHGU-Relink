"""Host-check real icon bridge/state and Java storage, without Android image decoding."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
workspace = Path(os.environ.get('MHGU_TEST_WORKSPACE', str(project)))
output = Path(os.environ.get('MHGU_TEST_OUTPUT', str(workspace / 'analysis/relink-touch-icon-bridge')))
output.mkdir(parents=True, exist_ok=True)
deps = workspace / 'analysis/host-test-deps'
sources = ['runtime/ui.c', 'runtime/app_actions.h',
           'android/app/src/main/java/com/mhgu/relink/GameActivity.java',
           'android/app/src/main/java/com/mhgu/relink/TouchIconImport.java',
           'tests/test_touch_icon_bridge.c', 'tests/TouchIconStoreHostTest.java',
           'tests/run_touch_icon_bridge.py']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot():
    return {name: sha(project / name) for name in sources}


def required_path(name, alternate=None):
    value = os.environ.get(name) or (os.environ.get(alternate) if alternate else None)
    if not value:
        raise RuntimeError(f'Set {name}' + (f' or {alternate}' if alternate else '') +
                           ' to the required local path before running this host check')
    return Path(value)


def run(name, command, env, expected=None):
    result = subprocess.run([str(value) for value in command], env=env, cwd=output,
                            capture_output=True, timeout=120)
    path = output / (name + '.log')
    path.write_bytes(result.stdout + result.stderr)
    report['logs'][name] = str(path)
    if result.returncode or expected:
        print(result.stdout.decode('utf-8', errors='replace'), end='')
        print(result.stderr.decode('utf-8', errors='replace'), end='')
    result.check_returncode()
    if expected:
        assert expected in result.stdout, name + ' did not complete all expected cases'


before = snapshot()
report = dict(passed=False, exit_code=1, source_root=str(project), source_sha256=before, logs={},
    method='Exact ui.c bridge/dispatch snippets with SDL mutex and threads; reflection calls production TouchIconImport.store compiled against android.jar',
    limits=['JNI string conversion is a narrow adapter fixture, not the Android JNI runtime.',
            'Android Bitmap decoding, content providers, picker UI, activity lifecycle and device rendering were not executed.',
            'Java storage receives predecoded TICO bytes. Its private production method is invoked unchanged.',
            'The C pthread calls are adapted to real SDL mutexes on Windows; this is a host synchronization test.'])
try:
    sdk = required_path('ANDROID_HOME', 'ANDROID_SDK_ROOT')
    ndk = Path(os.environ.get('ANDROID_NDK_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/ndk/27.0.12077973')))
    java = required_path('JAVA_HOME') / 'bin'
    android_jar = sdk / 'platforms/android-35/android.jar'
    source = (project / 'runtime/ui.c').read_text(encoding='utf-8')
    start = source.index('static pthread_mutex_t touch_icon_lock =')
    end = source.index('\nvoid ui_request_licenses(', start)
    android = source[start:end]
    pc_start = source.index('bool ui_request_touch_icon(void) { return false; }')
    pc_end = source.index('\n#endif', pc_start)
    pc = source[pc_start:pc_end]
    dispatch_start = source.index('        if (SDL_SetAtomicInt(&touch_icon_requested, 0)')
    dispatch_end = source.index('\n        if (action == 3)', dispatch_start)
    dispatch = source[dispatch_start:dispatch_end]
    assert 'chooseTouchIcon' in dispatch and 'touch_icon_result(-2, NULL)' in dispatch
    snippets = {'touch_icon_android_under_test.h': android,
                'touch_icon_pc_under_test.h': pc,
                'touch_icon_dispatch_under_test.h': 'static void dispatch_touch_icon_request(void) {\n' + dispatch + '\n}\n'}
    for name, text in snippets.items():
        (output / name).write_text(text, encoding='utf-8')
        assert (output / name).read_text(encoding='utf-8') == text
    report['source_extractions'] = {name: sha(output / name) for name in snippets}
    report['exact_source_snippets'] = True

    vcvars = required_path('VCVARS64')
    setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True,
                           text=True, check=True, timeout=60)
    env = os.environ.copy()
    for line in setup.stdout.splitlines():
        key, separator, value = line.partition('=')
        if separator and key:
            env[key] = value
    sdl = deps / 'SDL3-3.4.18'
    shutil.copy2(sdl / 'lib/x64/SDL3.dll', output / 'SDL3.dll')
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    exe = output / 'test-touch-icon-bridge.exe'
    run('native-bridge-build', [clang, '--target=x86_64-pc-windows-msvc', '-fuse-ld=link',
        '-std=gnu11', '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Werror',
        '-I' + str(output), '-I' + str(sdl / 'include'), '-I' + str(java.parent / 'include'),
        '-I' + str(java.parent / 'include/win32'), project / 'tests/test_touch_icon_bridge.c',
        sdl / 'lib/x64/SDL3.lib', '-o', exe], env)
    run('native-bridge', [exe], env, b'PASS_ICON_BRIDGE_GROUPS=5')
    classes = output / 'classes'; classes.mkdir(exist_ok=True)
    run('java-storage-build', [java / 'javac.exe', '--release', '11', '-encoding', 'UTF-8',
        '-cp', android_jar, '-d', classes,
        project / 'android/app/src/main/java/com/mhgu/relink/TouchIconImport.java',
        project / 'tests/TouchIconStoreHostTest.java'], env)
    run('java-storage', [java / 'java.exe', '-Djava.security.manager=allow', '-cp',
        str(classes) + os.pathsep + str(android_jar), 'TouchIconStoreHostTest', output], env,
        b'PASS_ICON_STORAGE_GROUPS=6')
    report.update(native_bridge_groups=5, java_storage_groups=6, android_bitmap_decode_tested=False,
        native_executable_sha256=sha(exe),
        java_production_class_sha256=sha(classes / 'com/mhgu/relink/TouchIconImport.class'))
    report['source_sha256_after'] = snapshot()
    report['source_unchanged'] = before == report['source_sha256_after']
    assert report['source_unchanged'], 'Icon bridge source changed during validation'
    report.update(passed=True, exit_code=0)
except Exception as error:
    report['error'] = f'{type(error).__name__}: {error}'
    print('FAIL:', report['error'], file=sys.stderr)
finally:
    path = output / 'icon-bridge-results.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Report:', path)
sys.exit(report['exit_code'])
