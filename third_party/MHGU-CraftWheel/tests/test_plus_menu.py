"""Replay the native Start menu in Unicorn using a locally supplied MHGU 1.4.0 main.bin.

Pass the unpacked, contiguous main module with text at offset zero, before relocation.
Requires devkitARM, unicorn and pyelftools.
"""
import argparse
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_CPU_ARM_CORTEX_A15,
)


class Guest:
    def __init__(self, main, harness):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
        for start, size in ((0, 0x2200000), (0x2ff0000, 0x200000), (0x4000000, 0x200000)):
            self.cpu.mem_map(start, size)
        self.cpu.mem_write(0, main)
        with harness.open("rb") as stream:
            elf = ELFFile(stream)
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD":
                    self.cpu.mem_write(segment["p_vaddr"], segment.data())
            self.symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}

    def word(self, address):
        return struct.unpack("<I", self.cpu.mem_read(address, 4))[0]

    def write(self, address, value):
        self.cpu.mem_write(address, struct.pack("<I", value))

    def got(self, load_pc, literal):
        return self.word(load_pc + 8 + self.word(literal))

    def call(self, name, *args):
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.cpu.reg_write(register, value)
        start = self.symbols[name] if isinstance(name, str) else name
        stop = 0x41fe000
        self.cpu.reg_write(UC_ARM_REG_SP, 0x41ff000)
        self.cpu.reg_write(UC_ARM_REG_LR, stop)
        self.cpu.emu_start(start, stop, count=150000)
        assert self.cpu.reg_read(UC_ARM_REG_PC) == stop, name
        return self.cpu.reg_read(UC_ARM_REG_R0)


def replay(main, harness, stock, fail_at):
    guest = Guest(main, harness)
    menu, heap, pad = 0x4000000, 0x4010000, 0x4060000
    old_rows, old_hints = heap + 0x3000, heap + 0x1000
    per_page, vtable = (7, 0x177aeb8) if stock == 13 else (8, 0x177e424)
    write, word, call = guest.write, guest.word, guest.call
    sym = guest.symbols
    # Supply the heavy GUI refresh, scene, and allocator effects. The cursor
    # constructor, both list-sizing routines, cursor input and root handler run
    # their original, unmodified ARM instructions from the supplied executable.
    write(menu, vtable)
    write(vtable + 0x94, sym["mock_refresh"])
    write(vtable + 0x7c, sym["stub"])
    write(vtable + 0x90, sym["stub"])  # stock item disabled for stock Confirm regression
    write(menu + 0xa8, old_rows)
    write(menu + 0xac, stock)
    write(menu + 0xc0, old_hints)
    write(menu + 0xc4, stock)
    for i in range(stock):
        call(0x54f9c0, old_hints + i * 12)
    write(0x188ca48, pad + 0x8000)
    guest.cpu.mem_write(pad + 0x8000 + 0xa5, bytes([8]))
    write(0x188c670, pad)
    write(heap, heap + 0x100)
    write(heap + 0x100 + 0x1c, sym["allocate"])
    write(heap + 0x100 + 0x34, sym["release"])
    # Check the same heap metadata, vtable slots and logical input pointer as
    # the game's two constructors and the real C++ vector's resize routine.
    assert guest.got(0x78f004, 0x78f288) == guest.got(0x677b7c, 0x677cc4) == 0x191eb18
    assert guest.got(0x625158, 0x625344) == 0x191eb18
    assert guest.got(0x66f88c, 0x66f9fc) == 0x188c670
    assert word(0x677b8c) == 0xe591301c and word(0x677c2c) == 0xe5912034
    allocation_class = 21
    write(0x191eb18 + 0x18, allocation_class << 23)
    write(guest.got(0x7aa484, 0x7aa494) + 4 * allocation_class, heap)
    assert call(0x7aa47c, 0x191eb18) == heap
    call(0x54ed74, menu + 0xd8)
    call(0x54edec, menu + 0xd8, per_page, 1, 0)
    call(0x54ee34, menu + 0xd8, stock, 3)
    guest.cpu.mem_write(menu + 0xf8, bytes(1))  # no cursor sound
    # Silence the separate native disabled-item sound for the stock Confirm test.
    guest.cpu.mem_write(0x54f260, struct.pack("<I", 0xe12fff1e))
    call("configure")
    write(sym["fail_at"], fail_at)
    call("plus_open", menu)
    assert call("plus_menu_active") == 1
    if fail_at:
        assert word(menu + 0xa8) == old_rows and word(menu + 0xc0) == old_hints
        assert word(menu + 0xc4) == stock
        assert guest.cpu.mem_read(menu + 0xea, 2) == struct.pack("<H", 2)
        call("plus_close", menu)
        return
    index = 2 * per_page
    assert word(menu + 0xac) == word(menu + 0xc4) == index + 1
    assert struct.unpack("<HH", guest.cpu.mem_read(menu + 0xea, 4)) == (3, index + 1)

    def input_frame(bits):
        write(pad + 0x6bfc, bits)
        return call("plus_input", menu)

    def check_page(page):
        assert guest.cpu.mem_read(menu + 0xe8, 2) == struct.pack("<H", page)

    assert input_frame(0x2000) == 0 and not call("plus_menu_take_open")
    input_frame(0x20)
    check_page(1)
    if stock == 13:
        for _ in range(6):
            input_frame(0x40)
        assert guest.cpu.mem_read(menu + 0xe5, 1) == bytes(1)
    input_frame(0x20)
    check_page(2)
    rows = word(menu + 0xa8)
    label = word(rows + index * 8 + 4)
    assert bytes(guest.cpu.mem_read(label, 64)).split(bytes(1), 1)[0].decode() == "配置快捷轮盘"
    assert word(rows + index * 8) & 255 == 1
    hint = word(menu + 0xc0) + index * 12
    assert guest.cpu.mem_read(hint + 4, 1) == bytes([255])
    input_frame(0x20)
    check_page(0)
    input_frame(0x80)
    check_page(2)
    assert input_frame(0x4000) == 4 and not call("plus_menu_take_open")
    assert input_frame(0x6000) == 4 and not call("plus_menu_take_open")
    assert input_frame(0x2000) == 4 and call("plus_menu_take_open") and not call("plus_menu_take_open")
    call("plus_close", menu)
    assert not call("plus_menu_active")
    call("plus_open", menu)
    assert word(menu + 0xa8) == rows
    call("plus_destroy", menu)
    assert not call("plus_menu_active")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="source tree containing wheel/plus_menu.inc")
    parser.add_argument("--main", type=Path, required=True, help="locally extracted MHGU 1.4.0 main.bin")
    parser.add_argument("--cc", default="arm-none-eabi-gcc")
    args = parser.parse_args()
    game = args.main.read_bytes()
    if game[0x66f868:0x66f86c] != bytes.fromhex("f0482de9"):
        parser.error("main.bin does not match the MHGU 1.4.0 native menu ABI")
    source = Path(__file__).with_name("test_plus_menu_harness.c")
    with TemporaryDirectory() as temp:
        harness = Path(temp) / "harness.elf"
        subprocess.run([args.cc, "-marm", "-march=armv7-a", "-mfloat-abi=soft", "-fno-builtin",
                        "-ffreestanding", "-nostdlib", "-O0", "-Wl,-Ttext=0x3000000", "-Wl,-e,configure",
                        "-iquote", str(args.project / "wheel"), "-x", "c", "-", "-o", str(harness)],
                       input=source.read_bytes(), check=True)
        for stock in (13, 16):
            for failure in (0, 1, 2):
                replay(game, harness, stock, failure)
    print("Native field and village menus passed paging, activation, cancellation, lifecycle and allocation-failure checks")


if __name__ == "__main__":
    main()
