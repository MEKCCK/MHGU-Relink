"""Run real Forge overlay/ImGui frames using MSVC, with host-rendered previews and no Android device."""
from pathlib import Path
import os
import hashlib
import json
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    if not install:
        raise SystemExit("MSVC C++ build tools are required.")
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    imgui = ROOT / "third_party/cimgui/imgui"
    workspace = Path(os.environ.get("MHGU_TEST_WORKSPACE", str(ROOT)))
    sdl = workspace / "analysis/host-test-deps/SDL3-3.4.18"
    vulkan = Path(os.environ.get("VULKAN_HEADERS", str(ROOT / "deps/src/SDL3-3.4.18/src/video/khronos")))
    if not (sdl / "include/SDL3/SDL.h").is_file() or not (vulkan / "vulkan/vulkan.h").is_file():
        raise SystemExit("SDL3 host dependencies and Vulkan headers are required.")
    configured_output = os.environ.get("MHGU_FORGE_TEST_OUTPUT")
    out = Path(configured_output) if configured_output else Path(tempfile.mkdtemp(prefix="mhgu-forge-test-"))
    out.mkdir(parents=True, exist_ok=True)
    # overlay.cpp's POSIX access check is unused by this headless harness.
    (out / "unistd.h").write_text("#include <io.h>\n#define access _access\n#define R_OK 4\n", encoding="ascii")
    sources = [ROOT / "tests/test_forge_close.cpp"] + [imgui / (name + ".cpp") for name in ["imgui", "imgui_draw", "imgui_tables", "imgui_widgets"]]
    inputs = [ROOT / "runtime/overlay.cpp", Path(__file__).resolve()] + sources
    hashes = lambda: {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    before = hashes()
    arguments = ["cl", "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/O1", "/Gy", "/DIMGUI_ENABLE_TEST_ENGINE"]
    arguments += ["/I" + str(path) for path in [out, imgui, sdl / "include", vulkan, ROOT / "runtime"]]
    arguments += [str(path) for path in sources]
    arguments += ["/Fe:" + str(out / "test-forge-close.exe"), "/link", "/OPT:REF", str(sdl / "lib/x64/SDL3.lib")]
    command = "call " + subprocess.list2cmdline([str(vcvars)]) + " >nul && " + subprocess.list2cmdline(arguments)
    build = subprocess.run(command, shell=True, cwd=out, capture_output=True)
    (out / "build.log").write_bytes(build.stdout + build.stderr)
    if build.returncode: print((build.stdout + build.stderr).decode("utf-8", errors="replace"))
    build.check_returncode()
    shutil.copy2(sdl / "lib/x64/SDL3.dll", out / "SDL3.dll")
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "dummy"
    env["MHGU_FORGE_TEST_FONT"] = str(ROOT / "android/app/src/main/assets/cjk.ttf")
    run = subprocess.run([str(out / "test-forge-close.exe")], cwd=out, env=env, capture_output=True, timeout=90)
    log = run.stdout + run.stderr
    (out / "regression.log").write_bytes(log)
    print(log.decode("utf-8", errors="replace"))
    assert hashes() == before, "Test inputs changed during validation"
    (out / "result.json").write_text(json.dumps(dict(passed=run.returncode == 0, exit_code=run.returncode,
        source_sha256=before, source_unchanged=True, previews=[p.name for p in out.glob("forge-phone-*.png")],
        device_tested=False), indent=2), encoding="utf-8")
    print("Evidence:", out)
    run.check_returncode()


if __name__ == "__main__":
    main()
