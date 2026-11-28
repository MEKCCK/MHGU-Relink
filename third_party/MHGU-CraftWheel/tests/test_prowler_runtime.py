"""Exercise the actual wheel include with MHGU's original ARM selector and support dispatch."""
import argparse
import struct
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

import lz4.block
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_R0, UC_ARM_REG_R1)

parser = argparse.ArgumentParser()
parser.add_argument("--main", type=Path, required=True)
parser.add_argument("--cc", required=True)
parser.add_argument("--include", type=Path, default=Path(__file__).with_name("prowler_runtime.inc"))
args = parser.parse_args()
main_base = 0x2000000
module = 0x5000000
heap = 0x6000000
u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
u.mem_map(main_base, 0x2200000)
u.mem_map(module, 0x40000)
u.mem_map(heap, 0x100000)
src = args.main.read_bytes()
for i in range(3):
    start, address, size = struct.unpack_from("<III", src, 16 + i*16)
    packed = struct.unpack_from("<I", src, 0x60 + 4*i)[0]
    data = lz4.block.decompress(src[start:start+packed], uncompressed_size=size)
    u.mem_write(main_base + address, data)


def put(at, value):
    u.mem_write(at, struct.pack("<I", value))


def word(at):
    return struct.unpack("<I", u.mem_read(at, 4))[0]


for got in (0x1838bb4, 0x1838bbc, 0x1838d80):
    put(main_base + got, main_base + word(main_base + got))
with TemporaryDirectory() as directory:
    c = Path(directory) / "entry.c"
    elf = c.with_suffix(".elf")
    c.write_text("""#include <stdint.h>
#include <stdbool.h>
static uint32_t base=0x2000000;
static bool quest=true;
static bool is_quest(void){return quest;}
#include "prowler_runtime.inc"
unsigned read_wheel(uint32_t *out){return prowler_read(out);}
unsigned use_wheel(unsigned i,unsigned id){return prowler_use(i,id);}
unsigned image_id(unsigned id){return prowler_display_action(id);}
unsigned owner(void){return prowler_owner;}
void set_quest(unsigned q){quest=q;}
""", encoding="utf8")
    subprocess.run([args.cc, "-Os", "-marm", "-march=armv7-a", "-ffreestanding",
                    "-fno-builtin", "-nostdlib", "-I", str(args.include.resolve().parent),
                    "-Wl,-Ttext=0x5000000,-e,read_wheel", str(c), "-o", str(elf)], check=True)
    with elf.open("rb") as stream:
        e = ELFFile(stream)
        funcs = {s.name:s["st_value"] for s in e.get_section_by_name(".symtab").iter_symbols()}
        for section in e.iter_sections():
            if section["sh_flags"] & 2 and section["sh_type"] != "SHT_NOBITS":
                u.mem_write(section["sh_addr"], section.data())

stop = heap + 0xff000
session, players, actor, other = [heap + n for n in (0x1000, 0x2000, 0x3000, 0x8000)]
record, vtable, output = [heap + n for n in (0xc004, 0xe000, 0x10000)]
put(main_base + 0x188ca48, session)
put(main_base + 0x1885b58, players)
put(main_base + 0x1885468, heap + 0xf000)
put(session+0x30, 1)
put(players+0x1c, other)
put(players+0x20, actor)
put(actor, vtable)
put(actor+0x13c4, record-4)
u.mem_write(actor+0x13cc, b"\x04")
put(actor+0x2500, 0x4000)
ids = bytes((31, 1, 9, 0, 56, 46, 55, 0))
u.mem_write(record+0x244, ids)
u.mem_write(record+0x274, b"CATID001")


def call(name, *values):
    for reg,value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1), values):
        u.reg_write(reg, value)
    u.reg_write(UC_ARM_REG_LR, stop)
    u.reg_write(UC_ARM_REG_SP, stop - 0x1000)
    u.emu_start(funcs[name], stop, count=12000)
    assert u.reg_read(UC_ARM_REG_PC) == stop
    return u.reg_read(UC_ARM_REG_R0)


def ret(at,value):
    u.mem_write(at, struct.pack("<II", 0xe3a00000|value,0xe12fff1e))
    u.ctl_remove_cache(at, at+8)


def snapshot():
    active = call("read_wheel", output)
    slots = struct.unpack("<8I",u.mem_read(output,32))
    return active, slots


assert snapshot() == (1, tuple(ids))
first = call("owner")
assert first and snapshot() == (1,tuple(ids)) and call("owner") == first
put(vtable+0x1b8, heap+0x11000)
ret(heap+0x11000,1)
assert call("image_id",46) == 57 and call("image_id",9) == 9
ret(heap+0x11000,0)
assert call("image_id",46) == 46
for mask in (2,4,8,12):
    # Only the actor's current status result is mocked. Stock equipped list
    # validation, Prowler resolver, Beast remapping and mailbox run unmodified.
    ret(main_base+0x12188dc,mask)
    assert not call("use_wheel",1,1) and word(actor+0x2500)==0x4000
ret(main_base+0x12188dc,0)
assert not call("use_wheel",2,1) and not call("use_wheel",8,1)
assert call("use_wheel",1,1) and word(actor+0x2500)==0x4800
assert u.mem_read(actor+0x32ac,2)==b"\x01\0"
assert not call("use_wheel",1,1)
put(actor+0x2500,0x4000)
ret(heap+0x11000,1)
assert call("use_wheel",5,46) and u.mem_read(actor+0x32ac,2)==b"\xfe\0"
put(actor+0x2500,0x4000)
u.mem_write(record+0x274,b"CATID002")
assert not call("use_wheel",1,1)
assert snapshot()==(1,tuple(ids)) and call("owner")!=first
u.mem_write(record+0x244,bytes(8))
assert snapshot()==(1,(0,)*8) and not call("use_wheel",1,1)
put(actor+0x13c4,0)
assert snapshot()==(1,(0,)*8) and not call("owner")
u.mem_write(actor+0x13cc,b"\0")
assert snapshot()==(0,(0,)*8)
u.mem_write(other+0x13cc,b"\x04")
assert snapshot()==(0,(0,)*8)
put(session+0x30,0xffffffff)
assert snapshot()==(0,(0,)*8)
put(main_base+0x65a1c4,0)
assert snapshot()==(0,(0,)*8)
print("Passed original ARM quest player/owner, equipped-slot order, gaps, hunter isolation,"
      " action status gating, duplicate request and Beast dispatch.")
