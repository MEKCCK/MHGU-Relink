from pathlib import Path
import os
import shutil
import socket
import subprocess
import time

here = Path(__file__).resolve().parent
project = here.parents[1]
import argparse
parser=argparse.ArgumentParser(description='Windows production C LDN + Rust transport integration tests')
parser.add_argument('--sdl-root',type=Path,required=True,help='SDL3 development package containing include/ and lib/x64/')
parser.add_argument('--ndk',type=Path,default=Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/ndk/27.0.12077973')
options=parser.parse_args()
deps=options.sdl_root.parent
out = here / 'output'
out.mkdir(exist_ok=True)
vcvars = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition('=')
    if sep and key:
        env[key] = value
clang = options.ndk/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
sdl = options.sdl_root
exe = out / 'hle-network.exe'
args = [str(clang), '-fuse-ld=link', '--target=x86_64-pc-windows-msvc', '-std=gnu17', '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-Wno-deprecated-declarations', '-I' + str(here), '-I' + str(project / 'runtime'), '-I' + str(sdl / 'include'), str(here / 'test_network_hle.c'), str(project / 'runtime' / 'config.c'), str(project / 'runtime' / 'config_store.c'), str(project / 'updater' / 'sha256.c'), str(sdl / 'lib' / 'x64' / 'SDL3.lib'), str(project / 'target' / 'debug' / 'mhgu_crossplay.lib'), 'ws2_32.lib', 'bcrypt.lib', 'ntdll.lib', 'userenv.lib', 'advapi32.lib', '-o', str(exe)]
args.insert(3, '-fms-runtime-lib=dll')
args.insert(args.index('-I' + str(project / 'runtime')), '-I' + str(deps))
for library in ['ws2_32.lib', 'bcrypt.lib', 'ntdll.lib', 'userenv.lib', 'advapi32.lib']:
    args.remove(library)
    args.extend(['-Xlinker', '/defaultlib:' + library])
for library in ['libcmt', 'libucrt', 'libvcruntime']:
    args.extend(['-Xlinker', '/nodefaultlib:' + library])
for library in ['ucrt', 'vcruntime']:
    args.extend(['-Xlinker', '/defaultlib:' + library])
build = subprocess.run(args, env=env, cwd=out, capture_output=True)
(out / 'build.log').write_bytes(build.stdout + build.stderr)
print(build.stdout.decode('utf-8', errors='replace') + build.stderr.decode('utf-8', errors='replace'), end='')
build.check_returncode()
shutil.copy2(sdl / 'lib' / 'x64' / 'SDL3.dll', out / 'SDL3.dll')
def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]
def run(name, args):
    result = subprocess.run([str(exe), *args], env=env, cwd=out, capture_output=True, timeout=35)
    (out / (name + '.log')).write_bytes(result.stdout + result.stderr)
    print(result.stdout.decode('utf-8', errors='replace'), end='')
    if result.returncode:
        print(result.stderr.decode('utf-8', errors='replace'))
    result.check_returncode()
run('lifecycle', ['lifecycle', str(free_port())])
run('edge', ['edge',str(free_port())])
for transport in ('native', 'ryu', 'room', 'mixed', 'remote'):
    port = str(free_port())
    ready = out / (transport+'-ready.flag')
    stop = out / (transport+'-stop.flag')
    ready.unlink(missing_ok=True)
    stop.unlink(missing_ok=True)
    extra=[transport] if transport in ('ryu','room','mixed') else []
    with (out / (transport+'-host.log')).open('wb') as host_log:
        host = subprocess.Popen([str(exe), 'server' if transport=='remote' else 'host', port, str(ready), str(stop), *extra], env=env, cwd=out, stdout=host_log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not ready.exists():
                if host.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Host did not become ready: '+transport)
                time.sleep(0.05)
            if transport=='remote':
                run('remote-create', ['remote',port,'ryu'])
                stop.touch()
            else:
                run(transport+'-station', ['station', port, str(stop), *(['room'] if transport=='mixed' else extra)])
            host.wait(timeout=10)
            if host.returncode:
                raise RuntimeError(f'Host failed: {host.returncode}; see {transport}-host.log')
        finally:
            if host.poll() is None:
                host.kill()
                host.wait(timeout=5)
    print((out/(transport+'-host.log')).read_text(encoding='utf-8', errors='replace'))
port=str(free_port())
ready, stop=out/'three-ready.flag', out/'three-stop.flag'
ready.unlink(missing_ok=True);stop.unlink(missing_ok=True)
processes=[]
try:
    for role, extra in [('three-host',['mixed']),('three-peer',['ryu']),('three-peer',['room'])]:
        log=(out/('three-'+extra[0]+'.log')).open('wb')
        command=[str(exe),role,port,*([str(ready),str(stop)] if role=='three-host' else [str(stop)]),*extra]
        process=subprocess.Popen(command,env=env,cwd=out,stdout=log,stderr=subprocess.STDOUT)
        processes.append((process,log))
        if role=='three-host':
            deadline=time.monotonic()+10
            while not ready.exists():
                if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('three-host not ready')
                time.sleep(0.05)
    for process, log in processes:
        process.wait(timeout=25)
        if process.returncode: raise RuntimeError('three-player failed, see three-*.log')
finally:
    for process, log in processes:
        if process.poll() is None: process.kill();process.wait(timeout=5)
        log.close()
run('delayed-mixed', ['delayed',str(free_port()),'mixed'])
for label in ('mixed','ryu','room'): print((out/('three-'+label+'.log')).read_text(encoding='utf-8',errors='replace'))
print('PASS C HLE lifecycle, native protocol2, RyuLdn, Room and Mixed packet roundtrips; logs:', out)
