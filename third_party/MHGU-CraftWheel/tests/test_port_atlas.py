"""Run texture registration, lifetime and draw-coordinate checks in ARM."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse
import subprocess
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_FPEXC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0

args=argparse.ArgumentParser()
args.add_argument("project",type=Path)
args.add_argument("--cc",default="arm-none-eabi-gcc")
a=args.parse_args()
with TemporaryDirectory() as temp:
    executable=Path(temp)/"atlas.elf"
    subprocess.run([a.cc,"-marm","-march=armv7-a","-mfpu=vfpv3-d16","-mfloat-abi=softfp",
                    "-fno-builtin","-ffreestanding","-nostdlib","-O2","-Wl,-e,run_test,-Ttext=0x10000",
                    "-I",str(a.project/"wheel"),"-I",str(a.project/"presets/vendor/cimgui-1.4"),
                    str(Path(__file__).with_suffix(".c")),"-lgcc","-o",str(executable)],check=True)
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    cpu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
    cpu.mem_map(0,0x1000000)
    elf=executable.read_bytes()
    entry,phoff=struct.unpack_from("<II",elf,24)
    phsize,count=struct.unpack_from("<HH",elf,42)
    for i in range(count):
        typ,offset,address,_,size=struct.unpack_from("<5I",elf,phoff+i*phsize)
        if typ==1 and size:cpu.mem_write(address,elf[offset:offset+size])
    cpu.reg_write(UC_ARM_REG_SP,0xf00000)
    cpu.reg_write(UC_ARM_REG_LR,0xf01000)
    cpu.emu_start(entry,0xf01000,count=65000000)
    assert cpu.reg_read(UC_ARM_REG_PC)==0xf01000
    assert cpu.reg_read(UC_ARM_REG_R0)==0,cpu.reg_read(UC_ARM_REG_R0)
print("ARM port texture tests passed: delayed allocation, opaque handles, failure recovery, controller, item and HD Prowler sprites, pixel copies, UVs, repeat draws")
