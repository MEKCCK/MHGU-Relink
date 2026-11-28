"""Compare the ARM wheel mapping against the host's fitted screen/HUD transform."""
import argparse
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory

from test_input_state import Controller


def float_bits(num):
    return struct.unpack("<I", struct.pack("<f", num))[0]


WRAPPER = """
#include <stdint.h>
#include <stdbool.h>
typedef struct {struct {float x,y;} DisplaySize;} ImGuiIO;
extern ImGuiIO* igGetIO_Nil(void);
extern bool forge_input_getTouch(float*,float*);
static unsigned base;
static bool ready;
#include "wheel_touch.inc"
void convert(float*x,float*y,float w,float h,unsigned rw,unsigned rh,unsigned uw,unsigned uh){
    wheel_touch_to_canvas(x,y,w,h,rw,rh,uw,uh);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--cc", default="arm-none-eabi-gcc")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        src = Path(temp) / "touch.c"
        src.write_text(WRAPPER, encoding="utf-8")
        elf = Path(temp) / "touch.elf"
        subprocess.run([
            args.cc, "-marm", "-march=armv7-a", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16",
            "-O2", "-ffreestanding", "-nostdlib", "-Wl,-e,convert,-Ttext=0x10000",
            "-I", str(args.project / "wheel"), str(src), "-lgcc", "-o", str(elf),
        ], check=True)
        c = Controller(elf)
        xy = c.BAG
        count = 0
        for w, h in ((1280,720), (1920,1080), (2560,1080), (3840,1080), (1600,1000), (2048,1536)):
            fit = min(w/1280, h/720)
            for rw, rh in ((1920,1080), (2560,1080), (3840,1080), (1600,1000)):
                a = rw/rh
                ph = min(w/a, h)
                for anchor in (16/9, 21/9, 32/9, a):
                    hud_aspect = min(a, anchor)
                    hud_w = ph * hud_aspect
                    # config.c sets an even HUD width with a minimum of 1280.
                    stored_width = max(1280, int(720*hud_aspect+.5) & ~1)
                    for gx, gy in ((200,155), (340,235), (900,230), (1070,480), (1190,585)):
                        px = (w-1280*fit)/2 + gx*fit
                        py = (h-720*fit)/2 + gy*fit
                        # runtime/forge.c window_to_screen: only HUD interior
                        # retains both coordinates; points in the black bars have no touch.
                        tx = int((px - (w-hud_w)/2)/hud_w*1280)
                        ty = int((py - (h-ph)/2)/ph*720)
                        if not(0 < tx < 1279 and 0 < ty < 719):
                            continue
                        c.arm.mem_write(xy, struct.pack("<ff", tx, ty))
                        c.call("convert", xy, xy+4, float_bits(w), float_bits(h), rw, rh, stored_width, 720)
                        ox, oy = struct.unpack("<ff", c.arm.mem_read(xy, 8))
                        assert abs(ox-gx) < 3.5 and abs(oy-gy) < 2.5, (w,h,rw,rh,anchor,gx,gy,ox,oy)
                        count += 1
        print(f"ARM touch round trips passed for {count} points across viewport, game aspect and HUD anchor combinations")


if __name__ == "__main__":
    main()
