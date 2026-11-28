"""Exercise real CPU font baking without an emulator or a GPU."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


def main():
    arg = argparse.ArgumentParser()
    arg.add_argument("--cxx", required=True, help="Host C++17 compiler (tested with i686 LLVM-MinGW)")
    arg.add_argument("--imgui", required=True, type=Path, help="wheelcore's imgui source directory")
    arg.add_argument("--presets", required=True, type=Path, help="Directory containing item_names.inc")
    arg.add_argument("--font", required=True, type=Path, help="Shipped gu_craft_wheel_font.ttc")
    arg.add_argument("--report", type=Path)
    args = arg.parse_args()
    src = Path(__file__).with_name("stress.cpp")
    with TemporaryDirectory() as d:
        exe = Path(d) / ("stress.exe" if os.name == "nt" else "stress")
        units = ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp"]
        platform_flags = ["-static", "-limm32", "-lgdi32", "-ldwmapi"] if os.name == "nt" else []
        subprocess.run([
            args.cxx, "-std=c++17", "-O1", "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS",
            "-DIMGUI_DISABLE_TTY_FUNCTIONS", "-I", args.imgui, "-I", args.presets, src,
            *(args.imgui / f for f in units), *platform_flags, "-o", exe,
        ], check=True)
        results = {}
        for mode, pair, sample, caps in [
            ("dynamic", (24, 36), 0, (0, 16, 24, 26)),
            ("fixed", (24, 36), 0, (0, 26)),
            ("fixed", (24, 36), 1, (0, 16, 24)),
            ("fixed", (30, 30), 1, (0, 16, 24)),
        ]:
            for cap in caps:
                config = f"{mode}/{pair[0]}-{pair[1]}/oversample-{sample}/budget-{cap}MiB"
                result = subprocess.run([exe, mode, args.font, str(cap), *map(str,pair), str(sample)],
                                        text=True, capture_output=True)
                expected_failure = mode == "dynamic" and cap != 0
                assert result.returncode == (9 if expected_failure else 0), (config, result)
                results[config] = result.stdout.splitlines()
        report = json.dumps(results, indent=2)
        if args.report:
            args.report.write_text(report + "\n", encoding="utf-8")
        else:
            print(report)


if __name__ == "__main__":
    main()
