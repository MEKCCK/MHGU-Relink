from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
project = here.parents[1]
ndk = Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk/ndk/27.0.12077973"
clang = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
vcvars = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

setup = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True)
env = os.environ.copy()
for line in setup.stdout.splitlines():
    key, sep, value = line.partition("=")
    if sep and key:
        env[key] = value

with tempfile.TemporaryDirectory(prefix="mhgu-mods-tests-") as temp:
    exe = Path(temp) / "test-mods.exe"
    args = [
        str(clang), "-fuse-ld=link", "--target=x86_64-pc-windows-msvc", "-std=gnu17",
        "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_DEPRECATE", "-Wno-deprecated-declarations",
        "-ffunction-sections", "-fdata-sections", "-I" + str(here), "-I" + str(project / "tests/network"),
        "-I" + str(project / "runtime"), str(here / "test_mods.c"), "-o", str(exe),
        "-Xlinker", "/opt:ref",
    ]
    build = subprocess.run(args, env=env, cwd=temp, capture_output=True, text=True)
    if build.returncode:
        raise SystemExit(build.stdout + build.stderr)
    run = subprocess.run([str(exe)], env=env, cwd=temp, capture_output=True, text=True)
    print(run.stdout, end="")
    if run.returncode:
        raise SystemExit(run.stderr or f"test executable exited with {run.returncode}")
