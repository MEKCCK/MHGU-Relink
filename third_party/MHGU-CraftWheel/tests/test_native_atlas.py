"""Run texture registration, lifetime and draw-coordinate checks in ARM."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse
import subprocess
from elftools.elf.elffile import ELFFile
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
    with executable.open("rb") as f:
        elf=ELFFile(f)
        for s in elf.iter_segments():
            if s["p_type"]=="PT_LOAD":cpu.mem_write(s["p_vaddr"],s.data())
        entry=elf.header["e_entry"]
    cpu.reg_write(UC_ARM_REG_SP,0xf00000)
    cpu.reg_write(UC_ARM_REG_LR,0xf01000)
    cpu.emu_start(entry,0xf01000,count=5000000)
    assert cpu.reg_read(UC_ARM_REG_PC)==0xf01000
    assert cpu.reg_read(UC_ARM_REG_R0)==0,cpu.reg_read(UC_ARM_REG_R0)
print("ARM texture tests passed: all tiers/styles/items, first frame, stable pages and multi-context shutdown/recreation")
