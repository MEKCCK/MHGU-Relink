"""Build and exercise the wheel's real ARM input controller (requires unicorn + pyelftools)."""
import argparse
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (
    UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
)


class Controller:
    BAG = 0x60000
    STACK = 0x70000
    RETURN = 0x80000

    def __init__(self, executable):
        self.arm = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.arm.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.arm.mem_map(0, 0x100000)
        with executable.open("rb") as f:
            elf = ELFFile(f)
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD":
                    self.arm.mem_write(segment["p_vaddr"], segment.data())
            self.symbols = {
                s.name: s["st_value"]
                for s in elf.get_section_by_name(".symtab").iter_symbols()
            }
        self.arm.mem_write(self.BAG, struct.pack("<HH", 9, 6) + bytes(156))

    def call(self, name, *args):
        registers = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
        for reg, value in zip(registers, args):
            self.arm.reg_write(reg, value & 0xffffffff)
        extra = b"".join(struct.pack("<I", arg & 0xffffffff) for arg in args[4:])
        if extra:
            self.arm.mem_write(self.STACK, extra)
        self.arm.reg_write(UC_ARM_REG_SP, self.STACK)
        self.arm.reg_write(UC_ARM_REG_LR, self.RETURN)
        self.arm.emu_start(self.symbols[name], self.RETURN, count=20000)
        assert self.arm.reg_read(UC_ARM_REG_PC) == self.RETURN, name

    def set_word(self, symbol, value):
        self.arm.mem_write(self.symbols[symbol], struct.pack("<I", value))

    def reset(self, opener=6, trigger=0):
        self.call("wheel_reset")
        self.set_word("open_key", opener)
        self.set_word("release_trigger", trigger)
        self.arm.mem_write(self.symbols["wheel"] + 20, struct.pack("<8I", *range(9, 17)))
        self.last = 0

    def step(self, ms, held, x=0, y=0, quest=True):
        pressed = held & ~self.last
        self.last = held
        self.call("wheel_step", int(quest), held, pressed, x, y, ms, self.BAG)
        return struct.unpack("<19I", self.arm.mem_read(self.symbols["wheel"], 76))


def hold_boundary(c):
    for opener in range(16):
        if opener in (5, 10):
            continue
        bit = 1 << opener
        for start in (0, 1000, 0xfffffff0):
            c.reset(opener)
            if bit != 64:
                assert c.step(start - 204, 64)[0] == 0
                assert c.step(start - 2, 64)[0] == 0
                c.step(start - 1, 0)
            assert c.step(start, bit)[0] == 0
            assert c.step(start + 199, bit)[0] == 0
            assert c.step(start + 200, bit, x=30000)[0:3] == (1, 0, 0)
            assert c.step(start + 400, bit, x=30000)[17] == 0
            c.step(start + 401, bit)
            assert c.step(start + 402, bit, x=30000)[17:] == (1, 10)
            assert c.step(start + 403, 0)[0] == 0
            assert c.step(start + 404, bit)[0] == 0
            assert c.step(start + 603, bit)[0] == 0
            assert c.step(start + 604, 0)[0] == 0
            assert c.step(start + 605, bit)[0] == 0
            assert c.step(start + 804, bit)[0] == 0
            assert c.step(start + 805, bit)[0] == 1


def craft_and_use(c):
    # Clocked repeats must occur once per elapsed 150 ms, even after a slow frame.
    for opener in (0, 2, 6, 9, 14):
        bit = 1 << opener
        for trigger in (0, 1, 2):
            c.reset(opener, trigger)
            c.step(0, bit)
            c.step(200, bit)
            assert c.step(201, bit, y=32000)[17:] == (1, 9)
            assert c.step(350, bit, y=32000)[17:] == (0, 0)
            assert c.step(351, bit, y=32000)[17:] == (1, 9)
            assert c.step(1900, bit, y=32000)[17:] == (1, 9)
            assert c.step(1901, bit, y=32000)[17:] == (0, 0)
            if trigger == 2:
                assert c.step(1902, bit | 32, x=32000)[17:] == (12, 10)
                c.step(1903, bit)
                assert c.step(1904, bit | 32)[17:] == (12, 0)
            else:
                action = c.step(1902, bit)[17:]
                assert action == ((9, 9) if trigger == 1 else (0, 0))
                assert c.step(1903, bit)[17:] == (0, 0)
            assert c.step(1910, 0)[0] == 0


def settings(c, android=False):
    for quest, chord in ((True, 64 | 16), (False, 64 | 8)):
        c.reset(opener=7)
        c.set_word("quick_enabled", 0)
        assert c.step(0, chord, quest=quest)[:3] == (1, 1, 0)
        assert c.step(1, 0, quest=quest)[17:] == (0, 0)
        c.set_word("preset_row", 4)
        def tap(bit):
            c.step(1, 0, quest=quest)
            return c.step(2, bit, quest=quest)
        tap(1 << 14)
        assert tap(1)[17:] == (11, 0)
        tap(1 << 13)
        assert tap(1)[17:] == (11, 11 if android else 12)
        for expected in ([0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 12] + ([11] if android else []) + [0]):
            tap(1 << 15)
            assert tap(1)[17:] == (11, expected)
        assert tap(2)[:3] == (0, 0, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="Wheel project containing wheel/wheel_state.c")
    parser.add_argument("--cc", default="arm-none-eabi-gcc")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        executable = Path(temp) / "wheel_state.elf"
        subprocess.run([
            args.cc, "-marm", "-march=armv7-a", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16",
            "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib",
            "-Wl,-e,wheel_step,-Ttext=0x10000", str(args.project / "wheel/wheel_state.c"),
            "-lgcc", "-o", str(executable),
        ], check=True)
        controller = Controller(executable)
        hold_boundary(controller)
        craft_and_use(controller)
        settings(controller, (args.project / "wheel/touch_mode.inc").exists())
    print(f"{args.project}: ARM hold timing, custom buttons, crafting, use and settings navigation passed")


if __name__ == "__main__":
    main()
