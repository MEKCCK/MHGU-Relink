"""Build the standalone release plugin from the corresponding source tree."""
import argparse
from hashlib import sha256
from pathlib import Path
import runpy
import subprocess
import sys

if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("target", choices=("port","android","eden","switch"))
    parser.add_argument("--cc", default="arm-none-eabi-gcc")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    project = root / args.target
    subprocess.run([sys.executable, str(project/"build.py"), "--cc", args.cc], check=True)
    data = (project/"wheel/gu_craft_wheel.nro").read_bytes()
    if args.target in ("eden","switch"):
        isolate = runpy.run_path(str(root/"isolate_runtime.py"))["isolate"]
        data = isolate(bytearray(data), plugin=True)[0]
        data[64:96] = bytes(32)
        data[64:96] = sha256(data).digest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(args.target, sha256(data).hexdigest(), flush=True)
