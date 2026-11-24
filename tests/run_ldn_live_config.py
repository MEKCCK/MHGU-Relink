"""Linux host end-to-end: saving RyuLdn settings mid-session applies without a restart.

Builds tests/test_ldn_live_config.c (the real runtime/net/ldn.c, config.c and the Rust
crossplay libraries) and runs it against tests/ryuldn_stub_server.py.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ldn-live-') as temporary:
    out = Path(temporary)
    subprocess.run(['cargo', 'build', '--release', '-p', 'mhgu-crossplay', '-p', 'mhgu-link-ffi'], cwd=project, check=True)
    cflags = subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True).split()
    libs = subprocess.check_output(['pkg-config', '--libs', 'sdl3'], text=True).split()
    exe = out / 'test'
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g',
                    '-I' + str(project / 'runtime'), '-I' + str(project / 'crates/mhgu-link-ffi/include'), *cflags,
                    str(project / 'tests/test_ldn_live_config.c'),
                    str(project / 'runtime/config.c'), str(project / 'runtime/config_store.c'),
                    str(project / 'updater/sha256.c'),
                    str(project / 'target/release/libmhgu_crossplay.a'),
                    str(project / 'target/release/libmhgu_link_ffi.a'),
                    *libs, '-lpthread', '-ldl', '-lm', '-o', str(exe)], cwd=project, check=True)
    port = 31000 + (os.getpid() % 1000)
    stub = subprocess.Popen([sys.executable, str(project / 'tests/ryuldn_stub_server.py'), str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1)
        subprocess.run([str(exe), str(out / 'test.ini'), str(port)], check=True)
    finally:
        stub.kill()
        stub.wait()
print('PASS: live RyuLdn config end-to-end (logs above)')
