"""Run real Forge overlay/ImGui frames using MSVC, without a device or renderer."""
from pathlib import Path
import os
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
    sdl = ROOT / "deps/out/arm64-v8a/sdl3/include"
    vulkan = ROOT / "deps/src/SDL3-3.4.18/src/video/khronos"
    if not (sdl / "SDL3/SDL.h").is_file() or not (vulkan / "vulkan/vulkan.h").is_file():
        raise SystemExit("Build the project's Android SDL3 dependency first (headers only are used).")
    with tempfile.TemporaryDirectory(prefix="mhgu-forge-test-") as directory:
        out = Path(directory)
        # overlay.cpp's POSIX access check is unused by this headless harness.
        (out / "unistd.h").write_text("#include <io.h>\n#define access _access\n#define R_OK 4\n", encoding="ascii")
        sources = [ROOT / "tests/test_forge_close.cpp"] + [imgui / (name + ".cpp") for name in ["imgui", "imgui_draw", "imgui_tables", "imgui_widgets"]]
        arguments = ["cl", "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/O1", "/Gy", "/DIMGUI_ENABLE_TEST_ENGINE"]
        arguments += ["/I" + str(path) for path in [out, imgui, sdl, vulkan, ROOT / "runtime"]]
        arguments += [str(path) for path in sources]
        arguments += ["/Fe:" + str(out / "test-forge-close.exe"), "/link", "/OPT:REF"]
        command = "call " + subprocess.list2cmdline([str(vcvars)]) + " >nul && " + subprocess.list2cmdline(arguments)
        subprocess.run(command, shell=True, check=True, cwd=out)
        subprocess.run([str(out / "test-forge-close.exe")], check=True, cwd=out)


if __name__ == "__main__":
    main()
