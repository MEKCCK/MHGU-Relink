"""Replay bag gestures in ARM, including all octants and trigger types."""
import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_FPEXC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0

cli=argparse.ArgumentParser(description=__doc__)
cli.add_argument("project",type=Path)
cli.add_argument("--cc",default="arm-none-eabi-gcc")
args=cli.parse_args()
with TemporaryDirectory() as scratch:
    elf=Path(scratch)/"test.elf"
    subprocess.run([args.cc,"-marm","-march=armv7-a","-mfpu=vfpv3-d16","-mfloat-abi=softfp",
                    "-ffreestanding","-fno-builtin","-nostdlib","-O2","-Wl,-e,test_backpack,-Ttext=0x10000",
                    "-I",str(args.project/"wheel"),str(Path(__file__).with_suffix(".c")),"-lgcc","-o",str(elf)],check=True)
    arm=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    arm.mem_map(0,0x100000)
    with elf.open("rb") as f:
        image=ELFFile(f)
        for seg in image.iter_segments():
            if seg["p_type"]=="PT_LOAD":arm.mem_write(seg["p_vaddr"],seg.data())
        entry=image.header["e_entry"]
    arm.reg_write(UC_ARM_REG_FPEXC,0x40000000)
    arm.reg_write(UC_ARM_REG_SP,0x70000)
    arm.reg_write(UC_ARM_REG_LR,0x80000)
    arm.emu_start(entry,0x80000,count=5000000)
    assert arm.reg_read(UC_ARM_REG_PC)==0x80000
    assert arm.reg_read(UC_ARM_REG_R0)==0, f"gesture assertion line {arm.reg_read(UC_ARM_REG_R0)}"
print("ARM bag gesture: eight directions, repeats, neutral/lift/outer triggers, cancellation, taps, saved position, town")
